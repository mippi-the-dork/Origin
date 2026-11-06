// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginHierarchyAuthoring.h"

#include "OriginBlueprintPivotEdit.h"
#include "OriginPivotComponent.h"
#include "OriginPivotViewportMode.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "OriginHierarchyAuthoring"

DEFINE_LOG_CATEGORY_STATIC(LogOriginHierarchyAuthoring, Log, All);

namespace
{
    struct FContext
    {
        UBlueprint* Blueprint = nullptr;
        USimpleConstructionScript* SCS = nullptr;
        USCS_Node* PivotNode = nullptr;
        USCS_Node* ParentNode = nullptr;
        UOriginPivotComponent* Pivot = nullptr;
    };

    /**
     * An SCS node's ComponentTemplate is unregistered authored data, not a live
     * component. After a native Undo/Redo, UE can restore an SCS template whose
     * ordinary movement setter leaves RelativeLocation/Rotation unchanged.
     *
     * Always try the normal API first. Only for a confirmed *unregistered SCS
     * template* that rejects the write, use the direct template-value setter.
     * The surrounding hierarchy transaction already calls Modify(), and a
     * structural Blueprint refresh happens after validation. Never use this
     * fallback for registered or ordinary world components.
     */
    bool SetAuthoredTemplateRelative(USceneComponent* Template, const FTransform& Desired,
        const TCHAR* Operation)
    {
        if (!IsValid(Template) || !Desired.IsValid() || Desired.ContainsNaN()) return false;
        if (Template->GetRelativeTransform().Equals(Desired, 1.e-5)) return true;

        Template->Modify();
        Template->SetRelativeTransform(Desired);
        if (Template->GetRelativeTransform().Equals(Desired, 1.e-5)) return true;

        if (!Template->IsTemplate() || Template->IsRegistered())
        {
            UE_LOG(LogOriginHierarchyAuthoring, Error,
                TEXT("%s: normal transform setter rejected value on non-SCS-template '%s'; refusing direct fallback."),
                Operation, *Template->GetPathName());
            return false;
        }

        // This is authored, unregistered template data. No collision, movement,
        // overlap, or physics side effects are valid here. A direct write is
        // appropriate only after the ordinary setter demonstrably failed.
        Template->SetRelativeTransform_Direct(Desired);
        const bool bAccepted = Template->GetRelativeTransform().Equals(Desired, 1.e-5);
        UE_LOG(LogOriginHierarchyAuthoring, Display,
            TEXT("%s: SCS template '%s' ignored SetRelativeTransform after Undo; direct authored-data fallback %s."),
            Operation, *Template->GetPathName(), bAccepted ? TEXT("accepted") : TEXT("failed"));
        return bAccepted;
    }

    bool HasAbsoluteAxes(const USceneComponent* Component)
    {
        return !IsValid(Component) || Component->IsUsingAbsoluteLocation() ||
            Component->IsUsingAbsoluteRotation() || Component->IsUsingAbsoluteScale();
    }

    bool ResolveContext(UOriginPivotComponent* Pivot, FContext& Out, FText& Reason)
    {
        Out = FContext();
        if (!IsValid(Pivot) || !GEditor || GEditor->PlayWorld ||
            FOriginPivotViewportMode::IsEditing(Pivot))
        {
            Reason = LOCTEXT("Busy", "Finish viewport pivot editing and exit Play in Editor before restructuring a Blueprint.");
            return false;
        }
        UBlueprint* BP = nullptr;
        USCS_Node* Node = nullptr;
        if (!OriginBlueprintPivot::Resolve(Pivot, BP, Node) ||
            !IsValid(BP) || !IsValid(BP->SimpleConstructionScript) || !IsValid(Node))
        {
            Reason = LOCTEXT("AuthoredOnly", "Hierarchy authoring requires an Origin Pivot authored in this Blueprint. Inherited and native components are not supported.");
            return false;
        }
        USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
        USCS_Node* Parent = SCS->FindParentNode(Node);
        if (!IsValid(Parent) || !IsValid(Parent->ComponentTemplate) ||
            !Parent->ComponentTemplate->IsA<USceneComponent>())
        {
            Reason = LOCTEXT("NoParent", "Place Origin Pivot beneath an authored Scene Component parent before restructuring its hierarchy.");
            return false;
        }
        if (HasAbsoluteAxes(Pivot) ||
            HasAbsoluteAxes(Cast<USceneComponent>(Parent->ComponentTemplate.Get())))
        {
            Reason = LOCTEXT("AbsoluteParent", "Absolute location, rotation or scale is unsupported on the pivot or its authored parent. No changes were made.");
            return false;
        }
        Out.Blueprint = BP;
        Out.SCS = SCS;
        Out.PivotNode = Node;
        Out.ParentNode = Parent;
        Out.Pivot = Pivot;
        return true;
    }

    bool SceneNodeInBranch(USCS_Node* Node, USCS_Node* ExpectedParent, USimpleConstructionScript* SCS,
        FText& Reason)
    {
        if (!IsValid(Node) || !IsValid(SCS) || SCS->FindParentNode(Node) != ExpectedParent ||
            !IsValid(Node->ComponentTemplate) ||
            !Node->ComponentTemplate->IsA<USceneComponent>())
        {
            Reason = LOCTEXT("NotDirectChild", "Select a directly authored scene-component variable in the correct branch. No hierarchy changes were made.");
            return false;
        }
        const USceneComponent* ChildTemplate = Cast<USceneComponent>(Node->ComponentTemplate.Get());
        if (HasAbsoluteAxes(ChildTemplate))
        {
            Reason = LOCTEXT("AbsoluteChild", "This component uses absolute location, rotation or scale. It cannot be safely reparented by Origin.");
            return false;
        }
        if (!Node->AttachToName.IsNone() || !ChildTemplate->GetAttachSocketName().IsNone())
        {
            Reason = LOCTEXT("SocketAttachment", "Socket or bone attachments are not yet supported for hierarchy authoring. No changes were made.");
            return false;
        }
        return true;
    }

    /** Preserve the target's parent-space transform exactly across reparenting.
     * Using Origin's existing conservative compensation rejects unsafe nonuniform-scale cases.
     */
    bool ComputePreservedRelative(const FTransform& OldParentInCommonFrame,
        const FTransform& NewParentInCommonFrame, const FTransform& OldChildRelative,
        FTransform& OutNewChildRelative, FText& Reason)
    {
        if (!OriginBlueprintPivot::CompensateChild(OldParentInCommonFrame,
            NewParentInCommonFrame, OldChildRelative, OutNewChildRelative))
        {
            Reason = LOCTEXT("UnsafeTransform", "Transform preservation requires valid, uniformly scaled component transforms. No changes were made.");
            return false;
        }
        return true;
    }

    void PrepareTransaction(const FContext& Context, USCS_Node* MovedNode)
    {
        // UE provides explicit SCS transaction snapshots. Capturing the hierarchy
        // (including child arrays and node/template state) matters when Undo is
        // followed by another structural edit; UObject::Modify alone did not
        // reliably restore the component-template associations in that sequence.
        // Transient automation fixtures without an active transaction do not
        // need this extra snapshot.
        if (Context.Blueprint->GetOutermost() != GetTransientPackage())
        {
            Context.SCS->SaveToTransactionBuffer();
            Context.ParentNode->SaveToTransactionBuffer();
            Context.PivotNode->SaveToTransactionBuffer();
            if (MovedNode) MovedNode->SaveToTransactionBuffer();
        }
        Context.Blueprint->Modify();
        Context.SCS->Modify();
        Context.ParentNode->Modify();
        Context.PivotNode->Modify();
        Context.Pivot->Modify();
        if (MovedNode)
        {
            MovedNode->Modify();
            if (UActorComponent* Template = MovedNode->ComponentTemplate.Get()) Template->Modify();
        }
    }

    FString DescribeSCSNode(const USCS_Node* Node)
    {
        return IsValid(Node) ? Node->GetVariableName().ToString() : TEXT("<none>");
    }

    /** Detailed postcondition check. Captured BEFORE rollback, while the failed topology
     * is still observable; never use an editor preview component as source of truth.
     */
    FString DiagnoseInsertion(const FContext& Context, USCS_Node* NewNode,
        USCS_Node* ChildNode, const FTransform& ChildBefore, const FTransform& NewPivotRelative)
    {
        USCS_Node* ActualPivotParent = Context.SCS->FindParentNode(NewNode);
        if (ActualPivotParent != Context.PivotNode)
        {
            return FString::Printf(TEXT("Inserted pivot '%s' parent mismatch: expected '%s', found '%s'"),
                *DescribeSCSNode(NewNode), *DescribeSCSNode(Context.PivotNode), *DescribeSCSNode(ActualPivotParent));
        }
        USCS_Node* ActualChildParent = Context.SCS->FindParentNode(ChildNode);
        if (ActualChildParent != NewNode)
        {
            return FString::Printf(TEXT("Child '%s' parent mismatch: expected '%s', found '%s'"),
                *DescribeSCSNode(ChildNode), *DescribeSCSNode(NewNode), *DescribeSCSNode(ActualChildParent));
        }
        const USceneComponent* Child = IsValid(ChildNode)
            ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
        const USceneComponent* InsertedPivot = IsValid(NewNode)
            ? Cast<USceneComponent>(NewNode->ComponentTemplate.Get()) : nullptr;
        if (!IsValid(Child) || !IsValid(InsertedPivot))
        {
            return FString::Printf(TEXT("Inserted pivot or child '%s' has no valid scene component template"),
                *DescribeSCSNode(ChildNode));
        }
        if (!InsertedPivot->GetRelativeTransform().Equals(NewPivotRelative, 1.e-3))
        {
            return FString::Printf(TEXT("Inserted pivot '%s' transform changed unexpectedly: expected %s, got %s"),
                *DescribeSCSNode(NewNode), *NewPivotRelative.ToString(),
                *InsertedPivot->GetRelativeTransform().ToString());
        }
        const FTransform Reconstructed = Child->GetRelativeTransform() * InsertedPivot->GetRelativeTransform();
        if (!Reconstructed.Equals(ChildBefore, 1.e-3))
        {
            return FString::Printf(TEXT("Child '%s' transform preservation failed: expected %s, got %s"),
                *DescribeSCSNode(ChildNode), *ChildBefore.ToString(),
                *Reconstructed.ToString());
        }
        return FString();
    }

    void NotifyStructure(const FContext& Context)
    {
        // This may reconstruct the Blueprint's preview. Do not use cached preview
        // components or source template pointers after this call.
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
    }
}

bool FOriginHierarchyAuthoring::CanEdit(UOriginPivotComponent* Pivot)
{
    FContext Context;
    FText Reason;
    return ResolveContext(Pivot, Context, Reason);
}

void FOriginHierarchyAuthoring::ListSiblingsAndChildren(UOriginPivotComponent* Pivot,
    TArray<FName>& OutSiblings, TArray<FName>& OutChildren)
{
    OutSiblings.Reset();
    OutChildren.Reset();
    FContext Context;
    FText Reason;
    if (!ResolveContext(Pivot, Context, Reason)) return;
    for (USCS_Node* Sibling : Context.ParentNode->GetChildNodes())
    {
        if (Sibling != Context.PivotNode && IsValid(Sibling) &&
            IsValid(Sibling->ComponentTemplate) && Sibling->ComponentTemplate->IsA<USceneComponent>() &&
            Sibling->AttachToName.IsNone() &&
            !HasAbsoluteAxes(Cast<USceneComponent>(Sibling->ComponentTemplate.Get()))) OutSiblings.Add(Sibling->GetVariableName());
    }
    for (USCS_Node* Child : Context.PivotNode->GetChildNodes())
    {
        if (IsValid(Child) && IsValid(Child->ComponentTemplate) &&
            Child->ComponentTemplate->IsA<USceneComponent>() && Child->AttachToName.IsNone() &&
            !HasAbsoluteAxes(Cast<USceneComponent>(Child->ComponentTemplate.Get())))
            OutChildren.Add(Child->GetVariableName());
    }
    OutSiblings.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
    OutChildren.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

bool FOriginHierarchyAuthoring::AdoptSibling(UOriginPivotComponent* Pivot, FName SiblingVariable, FText& Reason)
{
    FContext Context;
    if (!ResolveContext(Pivot, Context, Reason)) return false;
    USCS_Node* ChildNode = Context.SCS->FindSCSNode(SiblingVariable);
    if (ChildNode == Context.PivotNode)
    {
        Reason = LOCTEXT("CannotAdoptSelf", "A pivot cannot adopt itself.");
        return false;
    }
    if (!SceneNodeInBranch(ChildNode, Context.ParentNode, Context.SCS, Reason)) return false;
    USceneComponent* Child = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
    if (!IsValid(Child)) return false;
    const FTransform OldRelative = Child->GetRelativeTransform();
    FTransform NewRelative;
    if (!ComputePreservedRelative(FTransform::Identity, Pivot->GetRelativeTransform(),
        OldRelative, NewRelative, Reason)) return false;

    FScopedTransaction Transaction(LOCTEXT("AdoptTransaction", "Origin: Reparent Blueprint Component To Pivot"),
        Context.Blueprint->GetOutermost() != GetTransientPackage());
    PrepareTransaction(Context, ChildNode);
    Context.ParentNode->RemoveChildNode(ChildNode, false); // Keep node in SCS AllNodes.
    Context.PivotNode->AddChildNode(ChildNode, false);
    const bool bChildWritten = SetAuthoredTemplateRelative(Child, NewRelative, TEXT("Adopt sibling"));
    if (!bChildWritten || Context.SCS->FindParentNode(ChildNode) != Context.PivotNode ||
        !(Child->GetRelativeTransform() * Pivot->GetRelativeTransform()).Equals(OldRelative, 1.e-3))
    {
        Context.PivotNode->RemoveChildNode(ChildNode, false);
        Context.ParentNode->AddChildNode(ChildNode, false);
        SetAuthoredTemplateRelative(Child, OldRelative, TEXT("Adopt rollback"));
        Transaction.Cancel();
        Reason = LOCTEXT("AdoptPostcondition", "The SCS hierarchy did not accept the reparenting correctly. Original attachment was restored.");
        return false;
    }
    NotifyStructure(Context);
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginHierarchyAuthoring::PromoteChild(UOriginPivotComponent* Pivot, FName ChildVariable, FText& Reason)
{
    FContext Context;
    if (!ResolveContext(Pivot, Context, Reason)) return false;
    USCS_Node* ChildNode = Context.SCS->FindSCSNode(ChildVariable);
    if (!SceneNodeInBranch(ChildNode, Context.PivotNode, Context.SCS, Reason)) return false;
    USceneComponent* Child = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
    if (!IsValid(Child)) return false;
    const FTransform OldRelative = Child->GetRelativeTransform();
    FTransform NewRelative;
    if (!ComputePreservedRelative(Pivot->GetRelativeTransform(), FTransform::Identity,
        OldRelative, NewRelative, Reason)) return false;

    FScopedTransaction Transaction(LOCTEXT("PromoteTransaction", "Origin: Promote Blueprint Child Component"),
        Context.Blueprint->GetOutermost() != GetTransientPackage());
    PrepareTransaction(Context, ChildNode);
    Context.PivotNode->RemoveChildNode(ChildNode, false);
    Context.ParentNode->AddChildNode(ChildNode, false);
    const bool bChildWritten = SetAuthoredTemplateRelative(Child, NewRelative, TEXT("Promote child"));
    if (!bChildWritten || Context.SCS->FindParentNode(ChildNode) != Context.ParentNode ||
        !Child->GetRelativeTransform().Equals(OldRelative * Pivot->GetRelativeTransform(), 1.e-3))
    {
        Context.ParentNode->RemoveChildNode(ChildNode, false);
        Context.PivotNode->AddChildNode(ChildNode, false);
        SetAuthoredTemplateRelative(Child, OldRelative, TEXT("Promote rollback"));
        Transaction.Cancel();
        Reason = LOCTEXT("PromotePostcondition", "The SCS hierarchy did not accept the promotion correctly. Original attachment was restored.");
        return false;
    }
    NotifyStructure(Context);
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginHierarchyAuthoring::InsertPivotAboveChild(UOriginPivotComponent* Pivot,
    FName ChildVariable, FName& OutNewPivotVariable, FText& Reason)
{
    OutNewPivotVariable = NAME_None;
    FContext Context;
    if (!ResolveContext(Pivot, Context, Reason)) return false;
    USCS_Node* ChildNode = Context.SCS->FindSCSNode(ChildVariable);
    if (!SceneNodeInBranch(ChildNode, Context.PivotNode, Context.SCS, Reason)) return false;
    USceneComponent* Child = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
    if (!IsValid(Child)) return false;
    const FTransform ChildBefore = Child->GetRelativeTransform();

    // The inserted pivot appears at the child's previous origin and orientation.
    // Its scale stays 1; the child's original scale remains on the child.
    const FTransform NewPivotRelative(ChildBefore.GetRotation(), ChildBefore.GetLocation(), FVector::OneVector);
    FTransform NewChildRelative;
    if (!ComputePreservedRelative(FTransform::Identity, NewPivotRelative, ChildBefore,
        NewChildRelative, Reason)) return false;

    const FName NewName = Context.SCS->GenerateNewComponentName(UOriginPivotComponent::StaticClass(), TEXT("OriginPivot"));
    if (NewName.IsNone() || Context.SCS->FindSCSNode(NewName))
    {
        Reason = LOCTEXT("NameCollision", "Could not generate a unique Origin Pivot variable name.");
        return false;
    }
    FScopedTransaction Transaction(LOCTEXT("InsertTransaction", "Origin: Insert Blueprint Pivot Above Component"),
        Context.Blueprint->GetOutermost() != GetTransientPackage());
    PrepareTransaction(Context, ChildNode);
    USCS_Node* NewNode = Context.SCS->CreateNode(UOriginPivotComponent::StaticClass(), NewName);
    if (!IsValid(NewNode) || !IsValid(NewNode->ComponentTemplate))
    {
        Transaction.Cancel();
        Reason = LOCTEXT("CreationFailed", "Unreal could not create an authored Origin Pivot node. No hierarchy changes were applied.");
        return false;
    }
    NewNode->SetFlags(RF_Transactional);
    NewNode->Modify();
    UOriginPivotComponent* NewPivot = Cast<UOriginPivotComponent>(NewNode->ComponentTemplate.Get());
    if (!IsValid(NewPivot))
    {
        Transaction.Cancel();
        Reason = LOCTEXT("MissingTemplate", "The new SCS node has no Origin Pivot template. No hierarchy changes were applied.");
        return false;
    }
    NewPivot->Modify();
    const bool bInitialPivotWritten = SetAuthoredTemplateRelative(NewPivot, NewPivotRelative,
        TEXT("Insert pivot initial template"));
    Context.PivotNode->RemoveChildNode(ChildNode, false);
    Context.PivotNode->AddChildNode(NewNode, true); // This is a new SCS node.
    NewNode->AddChildNode(ChildNode, false);        // Child already exists in SCS AllNodes.

    // Record the pre-write state so a failed validation can distinguish a
    // compensation-plan error from an authored template that rejects or loses
    // the write. This is intentionally captured before the write, not inferred
    // from its final state after rollback.
    const USceneComponent* ChildBeforeWrite = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
    const FTransform ChildAfterTreeMove = IsValid(ChildBeforeWrite)
        ? ChildBeforeWrite->GetRelativeTransform() : FTransform::Identity;
    const FString ChildTemplatePathBeforeWrite = IsValid(ChildBeforeWrite)
        ? ChildBeforeWrite->GetPathName() : TEXT("<missing>");

    // After native Undo/Redo the authored node can have a different component
    // template than the pointer held before an SCS hierarchy mutation. Always
    // apply compensation to the CURRENT node template, not an obsolete template
    // object which may still be valid but is no longer authoritative.
    USceneComponent* CurrentChild = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
    UOriginPivotComponent* CurrentNewPivot = Cast<UOriginPivotComponent>(NewNode->ComponentTemplate.Get());
    const bool bPivotWritten = bInitialPivotWritten && IsValid(CurrentNewPivot) &&
        SetAuthoredTemplateRelative(CurrentNewPivot, NewPivotRelative, TEXT("Insert pivot current template"));
    if (IsValid(CurrentChild) && CurrentChild != Child)
    {
        UE_LOG(LogOriginHierarchyAuthoring, Display,
            TEXT("SCS child '%s' replaced its template during insertion; applying compensation to current template."),
            *ChildVariable.ToString());
    }
    const bool bChildWritten = IsValid(CurrentChild) &&
        SetAuthoredTemplateRelative(CurrentChild, NewChildRelative, TEXT("Insert compensated child"));
    // Capture the immediate setter result separately from postcondition checks.
    // A non-identity authored target that remains at its previous value is
    // evidence of an ignored/reverted template write, not bad SCS topology.
    const FTransform ChildAfterWrite = IsValid(CurrentChild)
        ? CurrentChild->GetRelativeTransform() : FTransform::Identity;
    const FString ChildTemplatePathAfterWrite = IsValid(CurrentChild)
        ? CurrentChild->GetPathName() : TEXT("<missing>");
    FString InsertionFailure;
    if (!bPivotWritten || !bChildWritten)
    {
        InsertionFailure = FString::Printf(TEXT("SCS template write was not accepted: inserted pivot=%d, child=%d"),
            bPivotWritten, bChildWritten);
    }
    else
    {
        InsertionFailure = DiagnoseInsertion(Context, NewNode, ChildNode,
            ChildBefore, NewPivotRelative);
    }
    if (!InsertionFailure.IsEmpty())
    {
        // Collect evidence before rollback, because rollback destroys the failed state.
        const USceneComponent* FinalChild = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
        const FTransform FinalChildRelative = IsValid(FinalChild)
            ? FinalChild->GetRelativeTransform() : FTransform::Identity;
        const bool bSetterAcceptedPlan = IsValid(CurrentChild) &&
            ChildAfterWrite.Equals(NewChildRelative, 1.e-3);
        const bool bTemplateChangedDuringTreeEdit = ChildBeforeWrite != Child ||
            CurrentChild != ChildBeforeWrite;
        UE_LOG(LogOriginHierarchyAuthoring, Error,
            TEXT("Insert Pivot Above '%s' in Blueprint '%s' failed: %s. Attempting rollback."),
            *ChildVariable.ToString(), *GetNameSafe(Context.Blueprint), *InsertionFailure);
        UE_LOG(LogOriginHierarchyAuthoring, Warning,
            TEXT("Insertion diagnostic | original=%s | planned child=%s | inserted pivot=%s | after reparent before write=%s | immediately after SetRelativeTransform=%s | final SCS node child=%s"),
            *ChildBefore.ToString(), *NewChildRelative.ToString(), *NewPivotRelative.ToString(),
            *ChildAfterTreeMove.ToString(), *ChildAfterWrite.ToString(), *FinalChildRelative.ToString());
        UE_LOG(LogOriginHierarchyAuthoring, Warning,
            TEXT("Insertion diagnostic | setter accepted plan=%d | template changed during tree edit=%d | template before=%s | template after=%s | final template=%s | registered=%d | template object=%d"),
            bSetterAcceptedPlan, bTemplateChangedDuringTreeEdit,
            *ChildTemplatePathBeforeWrite, *ChildTemplatePathAfterWrite,
            IsValid(FinalChild) ? *FinalChild->GetPathName() : TEXT("<missing>"),
            IsValid(FinalChild) && FinalChild->IsRegistered(),
            IsValid(FinalChild) && FinalChild->IsTemplate());
        NewNode->RemoveChildNode(ChildNode, false);
        Context.PivotNode->RemoveChildNode(NewNode, true);
        Context.PivotNode->AddChildNode(ChildNode, false);
        if (USceneComponent* RestoredChild = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()))
            SetAuthoredTemplateRelative(RestoredChild, ChildBefore, TEXT("Insert rollback"));
        Transaction.Cancel();

        // Do not claim restoration succeeded without checking it.
        const bool bRestoredParent = Context.SCS->FindParentNode(ChildNode) == Context.PivotNode;
        const USceneComponent* VerifiedRestoredChild = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
        const bool bRestoredTransform = IsValid(VerifiedRestoredChild) &&
            VerifiedRestoredChild->GetRelativeTransform().Equals(ChildBefore, 1.e-3);
        const bool bRemovedNewNode = Context.SCS->FindSCSNode(NewName) == nullptr;
        if (!bRestoredParent || !bRestoredTransform || !bRemovedNewNode)
        {
            UE_LOG(LogOriginHierarchyAuthoring, Error,
                TEXT("Insertion rollback was incomplete: child parent restored=%d, transform restored=%d, new node removed=%d. Do not save this Blueprint before reloading."),
                bRestoredParent, bRestoredTransform, bRemovedNewNode);
            Reason = FText::Format(LOCTEXT("InsertRollbackIncomplete",
                "Pivot insertion failed ({0}); rollback was incomplete. Do not save this Blueprint. Reopen it without saving and report the Output Log."),
                FText::FromString(InsertionFailure));
            return false;
        }
        Reason = FText::Format(LOCTEXT("InsertPostconditionDetailed",
            "Pivot insertion rejected ({0}). Original attachment was restored. See Output Log for details."),
            FText::FromString(InsertionFailure));
        return false;
    }
    OutNewPivotVariable = NewName;
    NotifyStructure(Context);
    Reason = FText::GetEmpty();
    return true;
}

#undef LOCTEXT_NAMESPACE
