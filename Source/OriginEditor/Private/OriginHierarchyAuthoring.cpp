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
        if (!Node->AttachToName.IsNone() || !Pivot->GetAttachSocketName().IsNone())
        {
            Reason = LOCTEXT("PivotSocketAttachment", "This Pivot is attached to a socket or bone. Component hierarchy restructuring requires a socket-free Pivot parent frame.");
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
            !HasAbsoluteAxes(Cast<USceneComponent>(Sibling->ComponentTemplate.Get())) &&
            Cast<USceneComponent>(Sibling->ComponentTemplate.Get())->GetAttachSocketName().IsNone())
            OutSiblings.Add(Sibling->GetVariableName());
    }
    for (USCS_Node* Child : Context.PivotNode->GetChildNodes())
    {
        if (IsValid(Child) && IsValid(Child->ComponentTemplate) &&
            Child->ComponentTemplate->IsA<USceneComponent>() && Child->AttachToName.IsNone() &&
            !HasAbsoluteAxes(Cast<USceneComponent>(Child->ComponentTemplate.Get())) &&
            Cast<USceneComponent>(Child->ComponentTemplate.Get())->GetAttachSocketName().IsNone())
            OutChildren.Add(Child->GetVariableName());
    }
    OutSiblings.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
    OutChildren.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

bool FOriginHierarchyAuthoring::AdoptSibling(UOriginPivotComponent* Pivot,
    FName SiblingVariable, FText& Reason)
{
    // Single-item and multi-item editing must never diverge in authoring rules.
    return AdoptSiblings(Pivot, { SiblingVariable }, Reason);
}

bool FOriginHierarchyAuthoring::PromoteChild(UOriginPivotComponent* Pivot,
    FName ChildVariable, FText& Reason)
{
    return PromoteChildren(Pivot, { ChildVariable }, Reason);
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


namespace
{
    enum class EOriginBatchDestination : uint8 { IntoPivot, ToParent, ToRoot };

    struct FOriginBatchMove
    {
        USCS_Node* Node = nullptr;
        USCS_Node* OldParent = nullptr;
        USCS_Node* NewParent = nullptr;
        FTransform OriginalRelative;
        FTransform PlannedRelative;
        FTransform BeforeInCommonFrame;
        FTransform OldParentInCommonFrame;
        FTransform NewParentInCommonFrame;
    };

    /** Resolve an authored SCS node into its common local authored root frame.
     * The root is treated as identity, and the root ITSELF is never moved.
     * Its attachment to an inherited or native parent therefore cancels out
     * for operations entirely inside the same authored root. We still reject
     * socket-dependent or absolute paths BELOW that root and cross-root moves.
     */
    bool BuildAuthoredRootFrame(const FContext& Context, USCS_Node* StartingNode,
        USCS_Node*& OutRoot, FTransform& OutNodeInRoot, FText& Reason)
    {
        OutRoot = nullptr;
        OutNodeInRoot = FTransform::Identity;
        USCS_Node* Current = StartingNode;
        TSet<const USCS_Node*> Seen;
        for (int32 Depth = 0; Depth < 128 && IsValid(Current); ++Depth)
        {
            if (Seen.Contains(Current)) break;
            Seen.Add(Current);
            if (Current->GetSCS() != Context.SCS) break;
            USCS_Node* Parent = Context.SCS->FindParentNode(Current);
            if (!IsValid(Parent))
            {
                if (!Context.SCS->GetRootNodes().Contains(Current)) break;
                // Root attachment metadata belongs to the unchanged outer
                // frame. It must not be overwritten or used to compensate
                // transforms within this common authored-root coordinate frame.
                OutRoot = Current;
                return true;
            }
            const USceneComponent* Template = Cast<USceneComponent>(Current->ComponentTemplate.Get());
            if (HasAbsoluteAxes(Template) || !Current->AttachToName.IsNone() ||
                !Template->GetAttachSocketName().IsNone() ||
                !Template->GetRelativeTransform().IsValid() ||
                Template->GetRelativeTransform().ContainsNaN()) break;
            const FVector Scale = Template->GetRelativeScale3D();
            if (Scale.GetMin() <= KINDA_SMALL_NUMBER ||
                !FMath::IsNearlyEqual(Scale.X, Scale.Y, 1.e-5) ||
                !FMath::IsNearlyEqual(Scale.Y, Scale.Z, 1.e-5)) break;
            OutNodeInRoot = OutNodeInRoot * Template->GetRelativeTransform();
            Current = Parent;
        }
        Reason = LOCTEXT("UnsupportedRootPath", "The operation requires a common authored Blueprint scene root and safe socket-free transforms below that root. Inherited/native components cannot be moved directly.");
        return false;
    }

    bool BuildRootFrame(const FContext& Context, USCS_Node*& OutRoot,
        FTransform& OutPivotInRoot, FText& Reason)
    {
        return BuildAuthoredRootFrame(Context, Context.PivotNode, OutRoot, OutPivotInRoot, Reason);
    }

    bool IsDescendantOrSelf(const FContext& Context, USCS_Node* PotentialDescendant,
        const USCS_Node* CandidateAncestor)
    {
        USCS_Node* Current = PotentialDescendant;
        TSet<const USCS_Node*> Seen;
        for (int32 Depth = 0; Depth < 128 && IsValid(Current); ++Depth)
        {
            if (Current == CandidateAncestor) return true;
            if (Seen.Contains(Current)) return true; // Invalid tree; reject conservatively.
            Seen.Add(Current);
            Current = Context.SCS->FindParentNode(Current);
        }
        return false;
    }

    /** Build a full cross-pivot move plan before an editor transaction begins. */
    bool BuildTransferMoves(const FContext& Context, USCS_Node* TargetNode,
        const TArray<FName>& Variables, TArray<FOriginBatchMove>& OutMoves, FText& Reason)
    {
        OutMoves.Reset();
        if (Variables.IsEmpty())
        {
            Reason = LOCTEXT("TransferEmpty", "Select at least one direct child to transfer.");
            return false;
        }
        if (!IsValid(TargetNode) || TargetNode == Context.PivotNode ||
            TargetNode->GetSCS() != Context.SCS ||
            !IsValid(TargetNode->ComponentTemplate) ||
            !TargetNode->ComponentTemplate->IsA<UOriginPivotComponent>())
        {
            Reason = LOCTEXT("TransferInvalidTarget", "Choose a different Origin Pivot authored in the same Blueprint.");
            return false;
        }
        USCS_Node* SourceRoot = nullptr;
        USCS_Node* TargetRoot = nullptr;
        FTransform SourceInRoot, TargetInRoot;
        if (!BuildAuthoredRootFrame(Context, Context.PivotNode, SourceRoot, SourceInRoot, Reason) ||
            !BuildAuthoredRootFrame(Context, TargetNode, TargetRoot, TargetInRoot, Reason)) return false;
        if (SourceRoot != TargetRoot)
        {
            Reason = LOCTEXT("TransferDifferentRoot", "Origin Pivots must share the same authored Blueprint scene root.");
            return false;
        }
        TSet<FName> Unique;
        for (const FName Variable : Variables)
        {
            if (Variable.IsNone() || Unique.Contains(Variable))
            {
                Reason = LOCTEXT("TransferDuplicate", "The transfer selection contains a duplicate or empty component variable.");
                return false;
            }
            Unique.Add(Variable);
            USCS_Node* ChildNode = Context.SCS->FindSCSNode(Variable);
            if (!SceneNodeInBranch(ChildNode, Context.PivotNode, Context.SCS, Reason)) return false;
            if (IsDescendantOrSelf(Context, TargetNode, ChildNode))
            {
                Reason = LOCTEXT("TransferCycle", "Cannot transfer a component beneath one of its own descendants.");
                return false;
            }
            const USceneComponent* Template = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
            if (!IsValid(Template) || !Template->GetRelativeTransform().IsValid() ||
                Template->GetRelativeTransform().ContainsNaN())
            {
                Reason = LOCTEXT("TransferInvalidTransform", "A selected authored component has an invalid transform.");
                return false;
            }
            FOriginBatchMove& Move = OutMoves.AddDefaulted_GetRef();
            Move.Node = ChildNode;
            Move.OldParent = Context.PivotNode;
            Move.NewParent = TargetNode;
            Move.OriginalRelative = Template->GetRelativeTransform();
            Move.OldParentInCommonFrame = SourceInRoot;
            Move.NewParentInCommonFrame = TargetInRoot;
            Move.BeforeInCommonFrame = Move.OriginalRelative * SourceInRoot;
            if (!ComputePreservedRelative(SourceInRoot, TargetInRoot,
                Move.OriginalRelative, Move.PlannedRelative, Reason)) return false;
        }
        return true;
    }

    bool BuildBatchMoves(const FContext& Context, const TArray<FName>& Variables,
        EOriginBatchDestination Destination, TArray<FOriginBatchMove>& OutMoves, FText& Reason)
    {
        OutMoves.Reset();
        if (Variables.IsEmpty())
        {
            Reason = LOCTEXT("BatchEmpty", "Select one or more authored components.");
            return false;
        }
        // Preflight the entire selection before beginning a transaction.
        TSet<FName> Unique;
        USCS_Node* Root = nullptr;
        FTransform PivotInRoot;
        if (Destination == EOriginBatchDestination::ToRoot &&
            !BuildRootFrame(Context, Root, PivotInRoot, Reason)) return false;
        USCS_Node* ExpectedParent = Destination == EOriginBatchDestination::IntoPivot
            ? Context.ParentNode : Context.PivotNode;
        USCS_Node* NewParent = Destination == EOriginBatchDestination::IntoPivot
            ? Context.PivotNode : (Destination == EOriginBatchDestination::ToParent ? Context.ParentNode : Root);
        if (!IsValid(NewParent) ||
            (NewParent == Context.PivotNode && Destination == EOriginBatchDestination::ToRoot))
        {
            Reason = LOCTEXT("InvalidBatchDestination", "There is no supported authored destination for this operation.");
            return false;
        }
        for (FName Name : Variables)
        {
            if (Name.IsNone() || Unique.Contains(Name))
            {
                Reason = LOCTEXT("DuplicateBatchVariables", "Selection contains an empty or duplicate component variable; no changes were made.");
                return false;
            }
            Unique.Add(Name);
            USCS_Node* Node = Context.SCS->FindSCSNode(Name);
            if (Node == Context.PivotNode ||
                !SceneNodeInBranch(Node, ExpectedParent, Context.SCS, Reason)) return false;
            const USceneComponent* Template = Cast<USceneComponent>(Node->ComponentTemplate.Get());
            if (!IsValid(Template) || !Template->GetRelativeTransform().IsValid())
            {
                Reason = LOCTEXT("InvalidBatchTemplate", "A selected component has an invalid authored transform.");
                return false;
            }
            FOriginBatchMove& Move = OutMoves.AddDefaulted_GetRef();
            Move.Node = Node;
            Move.OldParent = ExpectedParent;
            Move.NewParent = NewParent;
            Move.OriginalRelative = Template->GetRelativeTransform();
            Move.OldParentInCommonFrame = Destination == EOriginBatchDestination::IntoPivot
                ? FTransform::Identity : (Destination == EOriginBatchDestination::ToParent
                    ? Context.Pivot->GetRelativeTransform() : PivotInRoot);
            Move.NewParentInCommonFrame = Destination == EOriginBatchDestination::IntoPivot
                ? Context.Pivot->GetRelativeTransform() : FTransform::Identity;
            Move.BeforeInCommonFrame = Move.OriginalRelative * Move.OldParentInCommonFrame;
            if (!ComputePreservedRelative(Move.OldParentInCommonFrame,
                Move.NewParentInCommonFrame, Move.OriginalRelative, Move.PlannedRelative, Reason)) return false;
        }
        return true;
    }

    // Modified objects must include ALL moved SCS nodes and their templates before the
    // first tree edit, or a native Undo after a multi-selection can be incomplete.
    void PrepareBatchTransaction(const FContext& Context, const TArray<FOriginBatchMove>& Moves)
    {
        if (Context.Blueprint->GetOutermost() != GetTransientPackage())
        {
            Context.SCS->SaveToTransactionBuffer();
            Context.ParentNode->SaveToTransactionBuffer();
            Context.PivotNode->SaveToTransactionBuffer();
            for (const FOriginBatchMove& Move : Moves)
            {
                Move.Node->SaveToTransactionBuffer();
                if (Move.NewParent != Context.ParentNode && Move.NewParent != Context.PivotNode)
                    Move.NewParent->SaveToTransactionBuffer();
            }
        }
        Context.Blueprint->Modify();
        Context.SCS->Modify();
        Context.ParentNode->Modify();
        Context.PivotNode->Modify();
        Context.Pivot->Modify();
        for (const FOriginBatchMove& Move : Moves)
        {
            Move.Node->Modify();
            if (UActorComponent* Template = Move.Node->ComponentTemplate.Get()) Template->Modify();
            if (Move.NewParent != Context.ParentNode && Move.NewParent != Context.PivotNode)
                Move.NewParent->Modify();
        }
    }

    bool RollbackBatchMoves(const FContext& Context, const TArray<FOriginBatchMove>& Moves,
        int32 ChangedCount)
    {
        // Undo in reverse order and verify BOTH topology and the authored value.
        for (int32 Index = ChangedCount - 1; Index >= 0; --Index)
        {
            const FOriginBatchMove& Move = Moves[Index];
            if (Context.SCS->FindParentNode(Move.Node) == Move.NewParent)
                Move.NewParent->RemoveChildNode(Move.Node, false);
            if (Context.SCS->FindParentNode(Move.Node) != Move.OldParent)
                Move.OldParent->AddChildNode(Move.Node, false);
            if (USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get()))
                SetAuthoredTemplateRelative(Template, Move.OriginalRelative, TEXT("Batch rollback"));
        }
        for (const FOriginBatchMove& Move : Moves)
        {
            const USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            if (Context.SCS->FindParentNode(Move.Node) != Move.OldParent ||
                !IsValid(Template) || !Template->GetRelativeTransform().Equals(Move.OriginalRelative, 1.e-3))
                return false;
        }
        return true;
    }

    bool ExecuteBatchMoves(const FContext& Context, const TArray<FOriginBatchMove>& Moves,
        const FText& TransactionLabel, FText& Reason)
    {
        FScopedTransaction Transaction(TransactionLabel,
            Context.Blueprint->GetOutermost() != GetTransientPackage());
        PrepareBatchTransaction(Context, Moves);
        int32 ChangedCount = 0;
        for (const FOriginBatchMove& Move : Moves)
        {
            Move.OldParent->RemoveChildNode(Move.Node, false);
            Move.NewParent->AddChildNode(Move.Node, false);
            ++ChangedCount;
            USceneComponent* Current = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            const bool bWritten = IsValid(Current) &&
                SetAuthoredTemplateRelative(Current, Move.PlannedRelative, TEXT("Batch authored move"));
            if (!bWritten || Context.SCS->FindParentNode(Move.Node) != Move.NewParent ||
                !(Current->GetRelativeTransform() * Move.NewParentInCommonFrame).Equals(
                    Move.BeforeInCommonFrame, 1.e-3))
            {
                const bool bRestored = RollbackBatchMoves(Context, Moves, ChangedCount);
                Transaction.Cancel();
                if (!bRestored)
                {
                    UE_LOG(LogOriginHierarchyAuthoring, Error,
                        TEXT("Batch hierarchy rollback failed for '%s'; DO NOT SAVE the Blueprint."),
                        *GetNameSafe(Context.Blueprint));
                    Reason = LOCTEXT("BatchRollbackFailed", "Hierarchy edit failed and rollback was incomplete. Reopen this Blueprint without saving and check the Output Log.");
                }
                else
                    Reason = LOCTEXT("BatchApplyFailed", "Unreal rejected a component hierarchy change; the original hierarchy was restored.");
                return false;
            }
        }
        NotifyStructure(Context);
        Reason = FText::GetEmpty();
        return true;
    }

    bool RunBatch(UOriginPivotComponent* Pivot, const TArray<FName>& Variables,
        EOriginBatchDestination Destination, const FText& Label, FText& Reason)
    {
        FContext Context;
        if (!ResolveContext(Pivot, Context, Reason)) return false;
        TArray<FOriginBatchMove> Moves;
        if (!BuildBatchMoves(Context, Variables, Destination, Moves, Reason)) return false;
        return ExecuteBatchMoves(Context, Moves, Label, Reason);
    }
}

void FOriginHierarchyAuthoring::ListTransferTargets(UOriginPivotComponent* SourcePivot,
    TArray<FName>& OutTargets)
{
    OutTargets.Reset();
    FContext Context;
    FText Reason;
    if (!ResolveContext(SourcePivot, Context, Reason)) return;
    USCS_Node* SourceRoot = nullptr;
    FTransform SourceFrame;
    if (!BuildAuthoredRootFrame(Context, Context.PivotNode, SourceRoot, SourceFrame, Reason)) return;
    for (USCS_Node* Candidate : Context.SCS->GetAllNodes())
    {
        if (Candidate == Context.PivotNode || !IsValid(Candidate) ||
            !IsValid(Candidate->ComponentTemplate) ||
            !Candidate->ComponentTemplate->IsA<UOriginPivotComponent>()) continue;
        USCS_Node* TargetRoot = nullptr;
        FTransform TargetFrame;
        if (BuildAuthoredRootFrame(Context, Candidate, TargetRoot, TargetFrame, Reason) &&
            TargetRoot == SourceRoot)
            OutTargets.Add(Candidate->GetVariableName());
    }
    OutTargets.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

void FOriginHierarchyAuthoring::ListTransferTargetsForChildren(UOriginPivotComponent* SourcePivot,
    const TArray<FName>& SelectedVariables, TArray<FName>& OutTargets)
{
    // Discover the same candidates as before. Only selected children impose
    // additional constraints: showing a target that would certainly fail its
    // cycle/transform preflight makes the authoring UI unnecessarily confusing.
    ListTransferTargets(SourcePivot, OutTargets);
    if (SelectedVariables.IsEmpty() || OutTargets.IsEmpty()) return;

    FContext Context;
    FText Reason;
    if (!ResolveContext(SourcePivot, Context, Reason))
    {
        OutTargets.Reset();
        return;
    }
    OutTargets.RemoveAll([&](const FName TargetName)
    {
        USCS_Node* Target = Context.SCS->FindSCSNode(TargetName);
        TArray<FOriginBatchMove> Plan;
        FText PreflightReason;
        return !BuildTransferMoves(Context, Target, SelectedVariables, Plan, PreflightReason);
    });
}

bool FOriginHierarchyAuthoring::TransferChildrenToPivot(UOriginPivotComponent* SourcePivot,
    FName TargetPivotVariable, const TArray<FName>& Variables, FText& Reason)
{
    FContext Context;
    if (!ResolveContext(SourcePivot, Context, Reason)) return false;
    USCS_Node* TargetNode = Context.SCS->FindSCSNode(TargetPivotVariable);
    TArray<FOriginBatchMove> Moves;
    if (!BuildTransferMoves(Context, TargetNode, Variables, Moves, Reason)) return false;
    return ExecuteBatchMoves(Context, Moves,
        LOCTEXT("TransferTransaction", "Origin: Transfer Blueprint Components Between Pivots"), Reason);
}

bool FOriginHierarchyAuthoring::AdoptSiblings(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FText& Reason)
{
    return RunBatch(Pivot, Variables, EOriginBatchDestination::IntoPivot,
        LOCTEXT("BatchAdopt", "Origin: Reparent Blueprint Components To Pivot"), Reason);
}

bool FOriginHierarchyAuthoring::PromoteChildren(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FText& Reason)
{
    return RunBatch(Pivot, Variables, EOriginBatchDestination::ToParent,
        LOCTEXT("BatchPromote", "Origin: Promote Blueprint Components"), Reason);
}

bool FOriginHierarchyAuthoring::DetachChildrenToRoot(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FText& Reason)
{
    return RunBatch(Pivot, Variables, EOriginBatchDestination::ToRoot,
        LOCTEXT("BatchDetach", "Origin: Detach Blueprint Components To Root"), Reason);
}

bool FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& Reason)
{
    OutNewPivotVariable = NAME_None;
    FContext Context;
    if (!ResolveContext(Pivot, Context, Reason)) return false;
    if (Variables.Num() == 1) return InsertPivotAboveChild(Pivot, Variables[0], OutNewPivotVariable, Reason);
    TArray<FOriginBatchMove> Moves;
    if (!BuildBatchMoves(Context, Variables, EOriginBatchDestination::ToParent, Moves, Reason)) return false;
    // The new pivot is placed at the average local origin of the selected direct
    // children, with the enclosing Pivot's orientation and unit scale.
    FVector Mean = FVector::ZeroVector;
    for (const FOriginBatchMove& Move : Moves) Mean += Move.OriginalRelative.GetLocation();
    Mean /= Moves.Num();
    const FTransform NewPivotRelative(FQuat::Identity, Mean, FVector::OneVector);
    for (FOriginBatchMove& Move : Moves)
    {
        Move.NewParentInCommonFrame = NewPivotRelative;
        Move.BeforeInCommonFrame = Move.OriginalRelative;
        if (!ComputePreservedRelative(FTransform::Identity, NewPivotRelative,
            Move.OriginalRelative, Move.PlannedRelative, Reason)) return false;
        Move.NewParent = nullptr; // Assigned after the new node exists.
    }
    const FName NewName = Context.SCS->GenerateNewComponentName(
        UOriginPivotComponent::StaticClass(), TEXT("OriginPivot"));
    if (NewName.IsNone() || Context.SCS->FindSCSNode(NewName))
    {
        Reason = LOCTEXT("BatchInsertName", "Unreal could not generate a unique Origin Pivot variable name.");
        return false;
    }
    FScopedTransaction Transaction(LOCTEXT("BatchInsertTransaction", "Origin: Insert Shared Blueprint Pivot"),
        Context.Blueprint->GetOutermost() != GetTransientPackage());
    // Pre-snapshot the authored children before creating or parenting anything.
    TArray<FOriginBatchMove> InitialMoves = Moves;
    for (FOriginBatchMove& Move : InitialMoves) Move.NewParent = Context.PivotNode;
    PrepareBatchTransaction(Context, InitialMoves);
    USCS_Node* NewNode = Context.SCS->CreateNode(UOriginPivotComponent::StaticClass(), NewName);
    UOriginPivotComponent* NewTemplate = IsValid(NewNode)
        ? Cast<UOriginPivotComponent>(NewNode->ComponentTemplate.Get()) : nullptr;
    if (!IsValid(NewTemplate))
    {
        Transaction.Cancel();
        Reason = LOCTEXT("BatchInsertCreateFailed", "Unreal could not create a shared Origin Pivot component.");
        return false;
    }
    NewNode->SetFlags(RF_Transactional);
    NewNode->Modify();
    NewTemplate->Modify();
    if (!SetAuthoredTemplateRelative(NewTemplate, NewPivotRelative, TEXT("Shared pivot initial template")))
    {
        Transaction.Cancel();
        Reason = LOCTEXT("BatchInsertTransformFailed", "Unreal rejected the new pivot's authored transform.");
        return false;
    }
    Context.PivotNode->AddChildNode(NewNode, true);
    int32 ChangedCount = 0;
    for (FOriginBatchMove& Move : Moves)
    {
        Move.NewParent = NewNode;
        Context.PivotNode->RemoveChildNode(Move.Node, false);
        NewNode->AddChildNode(Move.Node, false);
        ++ChangedCount;
        USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
        if (!IsValid(Template) ||
            !SetAuthoredTemplateRelative(Template, Move.PlannedRelative, TEXT("Shared pivot child")) ||
            Context.SCS->FindParentNode(Move.Node) != NewNode ||
            !(Template->GetRelativeTransform() * NewTemplate->GetRelativeTransform()).Equals(
                Move.OriginalRelative, 1.e-3))
        {
            const bool bRestored = RollbackBatchMoves(Context, Moves, ChangedCount);
            Context.PivotNode->RemoveChildNode(NewNode, true);
            Transaction.Cancel();
            const bool bNodeRemoved = !Context.SCS->FindSCSNode(NewName);
            if (!bRestored || !bNodeRemoved)
            {
                UE_LOG(LogOriginHierarchyAuthoring, Error,
                    TEXT("Shared pivot rollback incomplete for '%s'; DO NOT SAVE."),
                    *GetNameSafe(Context.Blueprint));
                Reason = LOCTEXT("SharedPivotRollbackFailed", "Shared pivot insertion failed and rollback was incomplete. Reopen the Blueprint without saving.");
            }
            else Reason = LOCTEXT("SharedPivotRejected", "Unreal rejected the shared pivot insertion. Original hierarchy restored.");
            return false;
        }
    }
    OutNewPivotVariable = NewName;
    NotifyStructure(Context);
    Reason = FText::GetEmpty();
    return true;
}

#undef LOCTEXT_NAMESPACE
