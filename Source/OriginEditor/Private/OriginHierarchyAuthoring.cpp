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
    Child->SetRelativeTransform(NewRelative);
    if (Context.SCS->FindParentNode(ChildNode) != Context.PivotNode ||
        !(Child->GetRelativeTransform() * Pivot->GetRelativeTransform()).Equals(OldRelative, 1.e-3))
    {
        Context.PivotNode->RemoveChildNode(ChildNode, false);
        Context.ParentNode->AddChildNode(ChildNode, false);
        Child->SetRelativeTransform(OldRelative);
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
    Child->SetRelativeTransform(NewRelative);
    if (Context.SCS->FindParentNode(ChildNode) != Context.ParentNode ||
        !Child->GetRelativeTransform().Equals(OldRelative * Pivot->GetRelativeTransform(), 1.e-3))
    {
        Context.ParentNode->RemoveChildNode(ChildNode, false);
        Context.PivotNode->AddChildNode(ChildNode, false);
        Child->SetRelativeTransform(OldRelative);
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
    NewPivot->SetRelativeTransform(NewPivotRelative);
    Context.PivotNode->RemoveChildNode(ChildNode, false);
    Context.PivotNode->AddChildNode(NewNode, true); // This is a new SCS node.
    NewNode->AddChildNode(ChildNode, false);          // Child already exists in SCS AllNodes.
    Child->SetRelativeTransform(NewChildRelative);
    if (Context.SCS->FindParentNode(NewNode) != Context.PivotNode ||
        Context.SCS->FindParentNode(ChildNode) != NewNode ||
        !(Child->GetRelativeTransform() * NewPivotRelative).Equals(ChildBefore, 1.e-3))
    {
        NewNode->RemoveChildNode(ChildNode, false);
        Context.PivotNode->RemoveChildNode(NewNode, true);
        Context.PivotNode->AddChildNode(ChildNode, false);
        Child->SetRelativeTransform(ChildBefore);
        Transaction.Cancel();
        Reason = LOCTEXT("InsertPostcondition", "The SCS hierarchy did not accept the new pivot correctly. Original attachment was restored.");
        return false;
    }
    OutNewPivotVariable = NewName;
    NotifyStructure(Context);
    Reason = FText::GetEmpty();
    return true;
}

#undef LOCTEXT_NAMESPACE
