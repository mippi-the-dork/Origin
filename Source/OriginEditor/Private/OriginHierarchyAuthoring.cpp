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

    // An authored SCS node under a native/inherited component is represented as
    // a root SCS node with external-parent metadata. FindParentNode() is null:
    // the native Mesh is NOT an authored USCS_Node and must never be reparented.
    struct FExternalParentContext
    {
        UBlueprint* Blueprint = nullptr;
        USimpleConstructionScript* SCS = nullptr;
        USCS_Node* PivotNode = nullptr;
        UOriginPivotComponent* Pivot = nullptr;
        USceneComponent* ParentTemplate = nullptr;
    };

    bool ResolveExternalParentContext(UOriginPivotComponent* Pivot, FExternalParentContext& Out,
        FText& Reason)
    {
        Out = FExternalParentContext();
        if (!IsValid(Pivot) || !GEditor || GEditor->PlayWorld ||
            FOriginPivotViewportMode::IsEditing(Pivot))
        {
            Reason = LOCTEXT("ExternalParentBusy", "Finish viewport editing and exit PIE before editing Blueprint hierarchy.");
            return false;
        }
        UBlueprint* BP = nullptr;
        USCS_Node* PivotNode = nullptr;
        if (!OriginBlueprintPivot::Resolve(Pivot, BP, PivotNode) || !IsValid(BP) ||
            !IsValid(BP->SimpleConstructionScript) || !IsValid(PivotNode))
        {
            Reason = LOCTEXT("ExternalParentAuthoredOnly", "Select an Origin Pivot authored in this Blueprint, not a native or inherited Pivot.");
            return false;
        }
        USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
        if (SCS->FindParentNode(PivotNode) || !SCS->GetRootNodes().Contains(PivotNode) ||
            PivotNode->ParentComponentOrVariableName.IsNone())
        {
            Reason = LOCTEXT("ExternalParentNotRoot", "The selected Pivot is not an authored SCS root attached to a native or inherited parent.");
            return false;
        }
        USceneComponent* NativeOrInheritedParent = PivotNode->GetParentComponentTemplate(BP);
        if (!IsValid(NativeOrInheritedParent) || HasAbsoluteAxes(NativeOrInheritedParent) ||
            HasAbsoluteAxes(Pivot) || !PivotNode->AttachToName.IsNone() ||
            !Pivot->GetAttachSocketName().IsNone())
        {
            Reason = LOCTEXT("ExternalParentUnsupported", "The external parent must resolve to a safe Scene Component, and the reference Pivot must not use absolute axes or a socket attachment.");
            return false;
        }
        const FVector ParentScale = NativeOrInheritedParent->GetRelativeScale3D();
        if (ParentScale.GetMin() <= KINDA_SMALL_NUMBER ||
            !FMath::IsNearlyEqual(ParentScale.X, ParentScale.Y, 1.e-5) ||
            !FMath::IsNearlyEqual(ParentScale.Y, ParentScale.Z, 1.e-5))
        {
            Reason = LOCTEXT("ExternalParentScale", "The native or inherited parent has nonuniform or nonpositive scale. Origin cannot safely group these components.");
            return false;
        }
        Out.Blueprint = BP;
        Out.SCS = SCS;
        Out.PivotNode = PivotNode;
        Out.Pivot = Pivot;
        Out.ParentTemplate = NativeOrInheritedParent;
        return true;
    }

    bool IsEligibleExternalSibling(const FExternalParentContext& Context, USCS_Node* Candidate,
        FText& Reason)
    {
        if (!IsValid(Candidate) || Candidate == Context.PivotNode ||
            Candidate->GetSCS() != Context.SCS ||
            Context.SCS->FindParentNode(Candidate) ||
            !Context.SCS->GetRootNodes().Contains(Candidate) ||
            Candidate->ParentComponentOrVariableName != Context.PivotNode->ParentComponentOrVariableName ||
            Candidate->ParentComponentOwnerClassName != Context.PivotNode->ParentComponentOwnerClassName ||
            Candidate->bIsParentComponentNative != Context.PivotNode->bIsParentComponentNative ||
            Candidate->GetParentComponentTemplate(Context.Blueprint) != Context.ParentTemplate)
        {
            Reason = LOCTEXT("ExternalParentDifferent", "All selected components must be directly authored under the same native or inherited parent, with identical attachment ownership.");
            return false;
        }
        const USceneComponent* Template = Cast<USceneComponent>(Candidate->ComponentTemplate.Get());
        if (!IsValid(Template) || HasAbsoluteAxes(Template) ||
            !Template->GetAttachSocketName().IsNone() ||
            !Template->GetRelativeTransform().IsValid() ||
            Template->GetRelativeTransform().ContainsNaN())
        {
            Reason = LOCTEXT("ExternalParentInvalidChild", "An authored sibling has absolute axes, a template-level socket, or an invalid transform.");
            return false;
        }
        return true;
    }

    // Existing saved local-space poses capture the socket that was active at
    // the time of capture. Automatically changing a Pivot's socket must not
    // silently invalidate them. Creating a new Pivot leaves them untouched.
    bool HasAuthoredPivotPoses(const UOriginPivotComponent* Pivot)
    {
        if (!IsValid(Pivot)) return true;
        if (Pivot->OriginPivotConfiguration.Slots.Num() > 1 ||
            Pivot->OriginPivotConfiguration.ActiveSlotName != FName(TEXT("Default"))) return true;
        for (const FOriginPivotSlot& Slot : Pivot->OriginPivotConfiguration.Slots)
            if (Slot.bHasSavedRelativeTransform) return true;
        return false;
    }

    bool SceneNodeInBranch(USCS_Node* Node, USCS_Node* ExpectedParent, USimpleConstructionScript* SCS,
        FText& Reason, bool bAllowSocketHandoff = false)
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
        // SCS AttachToName is an authored socket/bone name on this node's
        // current parent. Ordinary reparenting cannot preserve that animated
        // frame. Insertion is different: the newly inserted Origin Pivot can
        // inherit the SAME attachment to the SAME parent, and the child then
        // becomes a socket-free child of the new Pivot. Never reinterpret a
        // separate component-template socket attachment.
        if (!ChildTemplate->GetAttachSocketName().IsNone())
        {
            Reason = LOCTEXT("TemplateSocketAttachment", "This component has a template-level socket attachment that Origin cannot safely transfer. No changes were made.");
            return false;
        }
        if (!bAllowSocketHandoff && !Node->AttachToName.IsNone())
        {
            Reason = LOCTEXT("SocketAttachment", "Reparenting a socket-attached component would change its socket frame. Use Insert Pivot Above instead, or leave its socket attachment unchanged.");
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
        USCS_Node* ChildNode, const FTransform& ChildBefore, const FTransform& NewPivotRelative,
        FName OriginalSocket)
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
        if (NewNode->AttachToName != OriginalSocket || !ChildNode->AttachToName.IsNone())
        {
            return FString::Printf(TEXT("Socket handoff mismatch: expected inserted socket '%s', got '%s', child socket '%s'"),
                *OriginalSocket.ToString(), *NewNode->AttachToName.ToString(), *ChildNode->AttachToName.ToString());
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

bool FOriginHierarchyAuthoring::CanGroupExternalParentSiblings(UOriginPivotComponent* Pivot)
{
    FExternalParentContext Context;
    FText Reason;
    return ResolveExternalParentContext(Pivot, Context, Reason);
}

void FOriginHierarchyAuthoring::ListExternalParentSiblings(UOriginPivotComponent* Pivot,
    TArray<FName>& OutAll, TArray<FName>& OutSocketAttached)
{
    OutAll.Reset();
    OutSocketAttached.Reset();
    FExternalParentContext Context;
    FText Reason;
    if (!ResolveExternalParentContext(Pivot, Context, Reason)) return;
    for (USCS_Node* Candidate : Context.SCS->GetRootNodes())
    {
        if (IsEligibleExternalSibling(Context, Candidate, Reason))
        {
            OutAll.Add(Candidate->GetVariableName());
            if (!Candidate->AttachToName.IsNone()) OutSocketAttached.Add(Candidate->GetVariableName());
        }
    }
    OutAll.Sort([](FName A, FName B) { return A.LexicalLess(B); });
    OutSocketAttached.Sort([](FName A, FName B) { return A.LexicalLess(B); });
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

void FOriginHierarchyAuthoring::ListInsertableSiblings(UOriginPivotComponent* Pivot,
    TArray<FName>& OutSiblings)
{
    OutSiblings.Reset();
    FContext Context;
    FText Reason;
    if (!ResolveContext(Pivot, Context, Reason)) return;
    for (USCS_Node* Node : Context.ParentNode->GetChildNodes())
    {
        if (Node == Context.PivotNode) continue;
        if (SceneNodeInBranch(Node, Context.ParentNode, Context.SCS, Reason, true) &&
            Cast<USceneComponent>(Node->ComponentTemplate.Get())->GetRelativeTransform().IsValid() &&
            !Cast<USceneComponent>(Node->ComponentTemplate.Get())->GetRelativeTransform().ContainsNaN())
            OutSiblings.Add(Node->GetVariableName());
    }
    OutSiblings.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

void FOriginHierarchyAuthoring::ListInsertableChildren(UOriginPivotComponent* Pivot,
    TArray<FName>& OutChildren)
{
    OutChildren.Reset();
    FContext Context;
    FText Reason;
    if (!ResolveContext(Pivot, Context, Reason)) return;
    for (USCS_Node* Child : Context.PivotNode->GetChildNodes())
    {
        if (SceneNodeInBranch(Child, Context.PivotNode, Context.SCS, Reason, true) &&
            Cast<USceneComponent>(Child->ComponentTemplate.Get())->GetRelativeTransform().IsValid() &&
            !Cast<USceneComponent>(Child->ComponentTemplate.Get())->GetRelativeTransform().ContainsNaN())
            OutChildren.Add(Child->GetVariableName());
    }
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
    if (!SceneNodeInBranch(ChildNode, Context.PivotNode, Context.SCS, Reason, true)) return false;
    const FName OriginalSocket = ChildNode->AttachToName;
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
    // Preserve the socket frame on the newly inserted node. This is an
    // exact metadata handoff, NOT an approximation of a skeletal bone pose.
    NewNode->AttachToName = OriginalSocket;
    ChildNode->AttachToName = NAME_None;

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
            ChildBefore, NewPivotRelative, OriginalSocket);
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
        ChildNode->AttachToName = OriginalSocket;
        if (USceneComponent* RestoredChild = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()))
            SetAuthoredTemplateRelative(RestoredChild, ChildBefore, TEXT("Insert rollback"));
        Transaction.Cancel();

        // Do not claim restoration succeeded without checking it.
        const bool bRestoredParent = Context.SCS->FindParentNode(ChildNode) == Context.PivotNode;
        const USceneComponent* VerifiedRestoredChild = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
        const bool bRestoredTransform = IsValid(VerifiedRestoredChild) &&
            VerifiedRestoredChild->GetRelativeTransform().Equals(ChildBefore, 1.e-3);
        const bool bRemovedNewNode = Context.SCS->FindSCSNode(NewName) == nullptr;
        const bool bRestoredSocket = ChildNode->AttachToName == OriginalSocket;
        if (!bRestoredParent || !bRestoredTransform || !bRemovedNewNode || !bRestoredSocket)
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
        FName OriginalSocket = NAME_None;
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
            Move.OriginalSocket = ChildNode->AttachToName;
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
            Move.OriginalSocket = Node->AttachToName;
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
            Move.Node->AttachToName = Move.OriginalSocket;
            if (USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get()))
                SetAuthoredTemplateRelative(Template, Move.OriginalRelative, TEXT("Batch rollback"));
        }
        for (const FOriginBatchMove& Move : Moves)
        {
            const USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            if (Context.SCS->FindParentNode(Move.Node) != Move.OldParent ||
                Move.Node->AttachToName != Move.OriginalSocket || !IsValid(Template) ||
                !Template->GetRelativeTransform().Equals(Move.OriginalRelative, 1.e-3))
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

namespace
{
    bool InsertSharedPivotAtParent(const FContext& Context, USCS_Node* DirectParent,
        const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& Reason)
    {
        OutNewPivotVariable = NAME_None;
        if (Variables.IsEmpty())
        {
            Reason = LOCTEXT("SharedInsertEmpty", "Select one or more authored child components to insert a shared Pivot.");
            return false;
        }
        // Socket-aware preflight: members must be direct, authored, safe and all
        // attached to the SAME socket on their unchanged parent. Both names and transforms are
        // evaluated in that identical socket frame. A mixed socket selection would
        // otherwise attach the new Pivot to the wrong moving bone.
        TArray<FOriginBatchMove> Moves;
        TSet<FName> Unique;
        FName SharedSocket = NAME_None;
        bool bHaveSocket = false;
        for (FName Name : Variables)
        {
            if (Name.IsNone() || Unique.Contains(Name))
            {
                Reason = LOCTEXT("SharedInsertDuplicate", "The insertion selection contains an empty or duplicate component variable.");
                return false;
            }
            Unique.Add(Name);
            USCS_Node* Node = Context.SCS->FindSCSNode(Name);
            if (Node == Context.PivotNode)
            {
                Reason = LOCTEXT("SharedInsertSourcePivot", "The active Origin Pivot cannot be included in its own sibling grouping operation.");
                return false;
            }
            if (!SceneNodeInBranch(Node, DirectParent, Context.SCS, Reason, true)) return false;
            const USceneComponent* Template = Cast<USceneComponent>(Node->ComponentTemplate.Get());
            if (!IsValid(Template) || !Template->GetRelativeTransform().IsValid() ||
                Template->GetRelativeTransform().ContainsNaN())
            {
                Reason = LOCTEXT("SharedInsertInvalid", "A selected child has an invalid authored transform.");
                return false;
            }
            if (!bHaveSocket)
            {
                SharedSocket = Node->AttachToName;
                bHaveSocket = true;
            }
            else if (SharedSocket != Node->AttachToName)
            {
                Reason = LOCTEXT("SharedInsertDifferentSockets", "A shared Pivot can only group components using the exact same socket (or all using none). Different moving socket frames cannot be merged without changing the assembly.");
                return false;
            }
            FOriginBatchMove& Move = Moves.AddDefaulted_GetRef();
            Move.Node = Node;
            Move.OldParent = DirectParent;
            Move.NewParent = nullptr;
            Move.OriginalSocket = Node->AttachToName;
            Move.OriginalRelative = Template->GetRelativeTransform();
        }
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
        for (FOriginBatchMove& Move : InitialMoves) Move.NewParent = DirectParent;
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
        DirectParent->AddChildNode(NewNode, true);
        NewNode->AttachToName = SharedSocket;
        int32 ChangedCount = 0;
        for (FOriginBatchMove& Move : Moves)
        {
            Move.NewParent = NewNode;
            DirectParent->RemoveChildNode(Move.Node, false);
            NewNode->AddChildNode(Move.Node, false);
            Move.Node->AttachToName = NAME_None;
            ++ChangedCount;
            USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            if (!IsValid(Template) ||
                !SetAuthoredTemplateRelative(Template, Move.PlannedRelative, TEXT("Shared pivot child")) ||
                Context.SCS->FindParentNode(Move.Node) != NewNode ||
                NewNode->AttachToName != SharedSocket || !Move.Node->AttachToName.IsNone() ||
                !(Template->GetRelativeTransform() * NewTemplate->GetRelativeTransform()).Equals(
                    Move.OriginalRelative, 1.e-3))
            {
                const bool bRestored = RollbackBatchMoves(Context, Moves, ChangedCount);
                DirectParent->RemoveChildNode(NewNode, true);
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
} // namespace

bool FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& Reason)
{
    OutNewPivotVariable = NAME_None;
    FContext Context;
    if (!ResolveContext(Pivot, Context, Reason)) return false;
    if (Variables.Num() == 1)
        return InsertPivotAboveChild(Pivot, Variables[0], OutNewPivotVariable, Reason);
    return InsertSharedPivotAtParent(Context, Context.PivotNode, Variables, OutNewPivotVariable, Reason);
}

namespace
{
// The two explicit UI actions share one transactional implementation: create
// a new Pivot, or consume the EMPTY selected Pivot as the group root.
bool GroupExternalParentSiblingsInternal(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& Reason, bool bReusePivot)
{
    OutNewPivotVariable = NAME_None;
    FExternalParentContext Context;
    if (!ResolveExternalParentContext(Pivot, Context, Reason)) return false;
    if (Variables.IsEmpty())
    {
        Reason = LOCTEXT("ExternalGroupEmpty", "Select at least one authored sibling to group.");
        return false;
    }

    // The outer frame is a native/inherited component, not an SCS node. It is
    // NEVER changed. All coordinates below are in the same parent/socket frame.
    struct FExternalMove
    {
        USCS_Node* Node = nullptr;
        FTransform OriginalRelative;
        FTransform PlannedRelative;
        FName OriginalSocket = NAME_None;
        FName OriginalParentName = NAME_None;
        FName OriginalOwnerName = NAME_None;
        bool bOriginalNative = false;
    };
    TArray<FExternalMove> Moves;
    TSet<FName> Seen;
    FName SharedSocket = NAME_None;
    bool bHasSocket = false;
    FVector Average = FVector::ZeroVector;
    for (FName Variable : Variables)
    {
        if (Variable.IsNone() || Seen.Contains(Variable))
        {
            Reason = LOCTEXT("ExternalGroupDuplicate", "Grouping selection contains an empty or duplicate component variable.");
            return false;
        }
        Seen.Add(Variable);
        USCS_Node* Node = Context.SCS->FindSCSNode(Variable);
        if (!IsEligibleExternalSibling(Context, Node, Reason)) return false;
        if (!bHasSocket) { SharedSocket = Node->AttachToName; bHasSocket = true; }
        else if (SharedSocket != Node->AttachToName)
        {
            Reason = LOCTEXT("ExternalGroupMixedSockets", "All siblings must use exactly the same socket on the same native/inherited parent (or all use none). No changes made.");
            return false;
        }
        const USceneComponent* Template = Cast<USceneComponent>(Node->ComponentTemplate.Get());
        FExternalMove& Move = Moves.AddDefaulted_GetRef();
        Move.Node = Node;
        Move.OriginalRelative = Template->GetRelativeTransform();
        Move.OriginalSocket = Node->AttachToName;
        Move.OriginalParentName = Node->ParentComponentOrVariableName;
        Move.OriginalOwnerName = Node->ParentComponentOwnerClassName;
        Move.bOriginalNative = Node->bIsParentComponentNative;
        Average += Move.OriginalRelative.GetLocation();
    }
    if (bReusePivot && (!Context.PivotNode->GetChildNodes().IsEmpty() ||
        HasAuthoredPivotPoses(Context.Pivot)))
    {
        Reason = LOCTEXT("ExternalAdoptNotEmpty", "The selected Pivot already has authored children or saved poses. Use Create New Pivot to keep its assembly and saved socket frames unchanged.");
        return false;
    }
    Average /= Moves.Num();
    const FTransform NewPivotRelative(FQuat::Identity, Average, FVector::OneVector);
    const FTransform OriginalPivotRelative = Context.Pivot->GetRelativeTransform();
    const FName OriginalPivotSocket = Context.PivotNode->AttachToName;
    for (FExternalMove& Move : Moves)
        if (!ComputePreservedRelative(FTransform::Identity, NewPivotRelative,
            Move.OriginalRelative, Move.PlannedRelative, Reason)) return false;

    const FName NewName = bReusePivot ? Context.PivotNode->GetVariableName() :
        Context.SCS->GenerateNewComponentName(UOriginPivotComponent::StaticClass(), TEXT("OriginPivot"));
    if (NewName.IsNone() || (!bReusePivot && Context.SCS->FindSCSNode(NewName)))
    {
        Reason = LOCTEXT("ExternalGroupName", "Unreal could not resolve a unique Pivot component variable.");
        return false;
    }

    FScopedTransaction Transaction(LOCTEXT("ExternalGroupTransaction", "Origin: Group Native-Parent Blueprint Siblings"),
        Context.Blueprint->GetOutermost() != GetTransientPackage());
    if (Context.Blueprint->GetOutermost() != GetTransientPackage())
    {
        Context.SCS->SaveToTransactionBuffer();
        Context.PivotNode->SaveToTransactionBuffer();
        for (const FExternalMove& Move : Moves) Move.Node->SaveToTransactionBuffer();
    }
    Context.Blueprint->Modify();
    Context.SCS->Modify();
    Context.PivotNode->Modify();
    Context.Pivot->Modify();
    for (const FExternalMove& Move : Moves)
    {
        Move.Node->Modify();
        if (UActorComponent* Template = Move.Node->ComponentTemplate.Get()) Template->Modify();
    }

    USCS_Node* NewNode = bReusePivot ? Context.PivotNode :
        Context.SCS->CreateNode(UOriginPivotComponent::StaticClass(), NewName);
    UOriginPivotComponent* NewTemplate = IsValid(NewNode)
        ? Cast<UOriginPivotComponent>(NewNode->ComponentTemplate.Get()) : nullptr;
    if (!IsValid(NewTemplate))
    {
        Transaction.Cancel();
        Reason = LOCTEXT("ExternalGroupCreate", "Unreal failed to create an Origin Pivot node.");
        return false;
    }
    NewNode->SetFlags(RF_Transactional);
    NewNode->Modify();
    NewTemplate->Modify();
    if (!SetAuthoredTemplateRelative(NewTemplate, NewPivotRelative, TEXT("External parent pivot initial")))
    {
        if (bReusePivot)
            SetAuthoredTemplateRelative(NewTemplate, OriginalPivotRelative, TEXT("External adoption setup rollback"));
        Transaction.Cancel();
        Reason = LOCTEXT("ExternalGroupTransform", "Unreal rejected the Pivot's authored transform.");
        return false;
    }
    if (!bReusePivot)
    {
        NewNode->bIsParentComponentNative = Context.PivotNode->bIsParentComponentNative;
        NewNode->ParentComponentOrVariableName = Context.PivotNode->ParentComponentOrVariableName;
        NewNode->ParentComponentOwnerClassName = Context.PivotNode->ParentComponentOwnerClassName;
    }
    NewNode->AttachToName = SharedSocket;
    if (!bReusePivot) Context.SCS->AddNode(NewNode);

    int32 Moved = 0;
    bool bSucceeded = Context.SCS->GetRootNodes().Contains(NewNode) &&
        NewNode->GetParentComponentTemplate(Context.Blueprint) == Context.ParentTemplate;
    if (bSucceeded)
    {
        for (FExternalMove& Move : Moves)
        {
            // RemoveNode removes a former SCS root from both RootNodes and
            // AllNodes. Reattach with bAddToAllNodes=true: otherwise the tree
            // looks correct in the Components panel but FindSCSNode, Blueprint
            // compilation, template/preview selection and Undo cannot resolve
            // the reparented children. (Alpha 5.8.1 regression.)
            Context.SCS->RemoveNode(Move.Node, false);
            NewNode->AddChildNode(Move.Node, true);
            Move.Node->bIsParentComponentNative = false;
            Move.Node->ParentComponentOrVariableName = NAME_None;
            Move.Node->ParentComponentOwnerClassName = NAME_None;
            Move.Node->AttachToName = NAME_None;
            ++Moved;
            USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            if (!IsValid(Template) ||
                !SetAuthoredTemplateRelative(Template, Move.PlannedRelative, TEXT("External parent grouped child")) ||
                Context.SCS->FindParentNode(Move.Node) != NewNode ||
                Context.SCS->GetRootNodes().Contains(Move.Node) ||
                !Context.SCS->GetAllNodes().Contains(Move.Node) ||
                Context.SCS->FindSCSNode(Move.Node->GetVariableName()) != Move.Node ||
                !Move.Node->AttachToName.IsNone() ||
                !(Template->GetRelativeTransform() * NewTemplate->GetRelativeTransform()).Equals(
                    Move.OriginalRelative, 1.e-3))
            {
                bSucceeded = false;
                break;
            }
        }
    }
    if (!bSucceeded)
    {
        // Explicit recovery is needed in addition to cancelling the Undo
        // transaction: a failed authoring operation must not leave the live
        // Blueprint tree partially changed or require the user to click Undo.
        bool bRestored = true;
        for (int32 Index = Moved - 1; Index >= 0; --Index)
        {
            const FExternalMove& Move = Moves[Index];
            if (Context.SCS->FindParentNode(Move.Node) == NewNode)
                NewNode->RemoveChildNode(Move.Node, true);
            Move.Node->ParentComponentOrVariableName = Move.OriginalParentName;
            Move.Node->ParentComponentOwnerClassName = Move.OriginalOwnerName;
            Move.Node->bIsParentComponentNative = Move.bOriginalNative;
            Move.Node->AttachToName = Move.OriginalSocket;
            if (!Context.SCS->GetRootNodes().Contains(Move.Node))
                Context.SCS->AddNode(Move.Node);
            USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            bRestored &= IsValid(Template) &&
                SetAuthoredTemplateRelative(Template, Move.OriginalRelative, TEXT("External group rollback"));
        }
        if (!bReusePivot && Context.SCS->GetRootNodes().Contains(NewNode))
            Context.SCS->RemoveNode(NewNode, false);
        if (bReusePivot)
        {
            NewNode->AttachToName = OriginalPivotSocket;
            bRestored &= SetAuthoredTemplateRelative(NewTemplate, OriginalPivotRelative,
                TEXT("External existing pivot rollback"));
        }
        for (const FExternalMove& Move : Moves)
        {
            const USceneComponent* Template = Cast<USceneComponent>(Move.Node->ComponentTemplate.Get());
            bRestored &= Context.SCS->GetRootNodes().Contains(Move.Node) &&
                Move.Node->ParentComponentOrVariableName == Move.OriginalParentName &&
                Move.Node->ParentComponentOwnerClassName == Move.OriginalOwnerName &&
                Move.Node->bIsParentComponentNative == Move.bOriginalNative &&
                Move.Node->AttachToName == Move.OriginalSocket &&
                IsValid(Template) && Template->GetRelativeTransform().Equals(Move.OriginalRelative, 1.e-3);
        }
        bRestored &= bReusePivot
            ? Context.SCS->FindSCSNode(NewName) == Context.PivotNode
            : !Context.SCS->FindSCSNode(NewName);
        Transaction.Cancel();
        if (!bRestored)
        {
            UE_LOG(LogOriginHierarchyAuthoring, Error,
                TEXT("External parent grouping rollback incomplete for '%s'. DO NOT SAVE."),
                *GetNameSafe(Context.Blueprint));
            Reason = LOCTEXT("ExternalGroupRollbackFailed", "Native-parent grouping failed and rollback was incomplete. Reopen the Blueprint without saving and report the Output Log.");
        }
        else Reason = LOCTEXT("ExternalGroupRejected", "Unreal rejected native-parent grouping. The original component hierarchy was restored.");
        return false;
    }

    OutNewPivotVariable = NewName;
    FContext NotifyContext;
    NotifyContext.Blueprint = Context.Blueprint;
    NotifyStructure(NotifyContext);
    Reason = FText::GetEmpty();
    return true;
}
} // namespace

bool FOriginHierarchyAuthoring::GroupExternalParentSiblings(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& Reason)
{
    return GroupExternalParentSiblingsInternal(Pivot, Variables, OutNewPivotVariable, Reason, false);
}

bool FOriginHierarchyAuthoring::CanAdoptExternalParentSiblings(UOriginPivotComponent* Pivot)
{
    FExternalParentContext Context;
    FText Reason;
    return ResolveExternalParentContext(Pivot, Context, Reason) &&
        Context.PivotNode->GetChildNodes().IsEmpty() && !HasAuthoredPivotPoses(Context.Pivot);
}

bool FOriginHierarchyAuthoring::AdoptExternalParentSiblings(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FText& Reason)
{
    FName UsedPivot;
    return GroupExternalParentSiblingsInternal(Pivot, Variables, UsedPivot, Reason, true);
}

bool FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(UOriginPivotComponent* Pivot,
    const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& Reason)
{
    OutNewPivotVariable = NAME_None;
    FContext Context;
    if (!ResolveContext(Pivot, Context, Reason))
        return GroupExternalParentSiblings(Pivot, Variables, OutNewPivotVariable, Reason);
    return InsertSharedPivotAtParent(Context, Context.ParentNode, Variables, OutNewPivotVariable, Reason);
}

#undef LOCTEXT_NAMESPACE
