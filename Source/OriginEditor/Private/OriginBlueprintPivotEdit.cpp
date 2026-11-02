// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginBlueprintPivotEdit.h"

#include "OriginPerformance.h"

#include "OriginPivotComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#define LOCTEXT_NAMESPACE "OriginBlueprintPivotEdit"

namespace
{
    constexpr double TransformTolerance = 1.e-5;
    bool ValidScale(const FVector& Scale)
    {
        return FMath::IsFinite(Scale.X) && FMath::IsFinite(Scale.Y) && FMath::IsFinite(Scale.Z)
            && Scale.GetMin() > KINDA_SMALL_NUMBER
            && FMath::IsNearlyEqual(Scale.X, Scale.Y, TransformTolerance)
            && FMath::IsNearlyEqual(Scale.Y, Scale.Z, TransformTolerance);
    }
    void SetTemplateRelative(USceneComponent* Component, const FTransform& Relative)
    {
        if (IsValid(Component))
        {
            if (!Component->GetRelativeTransform().Equals(Relative, 1.e-5))
                Component->SetRelativeTransform(Relative);
        }
    }
}

namespace
{
    /** SCS component templates commonly have the generated class as their Outer,
        *not* their USCS_Node. Do not depend on GetTypedOuter<USCS_Node>(). */
    UBlueprint* FindOwningBlueprint(const UObject* Object)
    {
        for (const UObject* Outer = Object; Outer; Outer = Outer->GetOuter())
        {
            if (const UBlueprint* BP = Cast<UBlueprint>(Outer))
                return const_cast<UBlueprint*>(BP);
            if (const UClass* Class = Cast<UClass>(Outer))
            {
                if (UBlueprint* BP = UBlueprint::GetBlueprintFromClass(Class))
                    return BP;
            }
        }
        return nullptr;
    }

    USCS_Node* FindAuthoredNode(UBlueprint* BP, const UOriginPivotComponent* Component,
        bool bMatchPreview)
    {
        if (!IsValid(BP) || !IsValid(BP->SimpleConstructionScript) || !IsValid(Component)) return nullptr;
        for (USCS_Node* Candidate : BP->SimpleConstructionScript->GetAllNodes())
        {
            if (!IsValid(Candidate)) continue;
            if (Candidate->ComponentTemplate.Get() == Component ||
                (bMatchPreview && Candidate->EditorComponentInstance.Get() == Component))
                return Candidate;
        }
        return nullptr;
    }
}

bool OriginBlueprintPivot::IsUnmodifiedPreviewTransform(const FTransform& Authored, const FTransform& Preview)
{
    // Compare relative values, not world transforms. The Blueprint Editor places
    // its preview Actor independently of the component's authored parent frame.
    return Authored.IsValid() && Preview.IsValid() &&
        !Authored.ContainsNaN() && !Preview.ContainsNaN() && Authored.Equals(Preview, 1.e-3);
}

USceneComponent* OriginBlueprintPivot::FindPreviewComponent(UBlueprint* Blueprint, USCS_Node* Node)
{
    if (!IsValid(Blueprint) || !IsValid(Blueprint->SimpleConstructionScript) ||
        !IsValid(Node) || Node->GetSCS() != Blueprint->SimpleConstructionScript ||
        !IsValid(Node->ComponentTemplate) || !IsValid(Node->ComponentClass)) return nullptr;

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    // This is the editor's active preview Actor, not an Actor spawned by Origin.
    AActor* PreviewActor = SCS->GetComponentEditorActorInstance();
    if (!IsValid(PreviewActor))
    {
        // Some Blueprint editor configurations publish only the transient node
        // association. Use it to find the owner, never a global actor search.
        if (const USceneComponent* Existing = Node->EditorComponentInstance.Get())
            PreviewActor = Existing->GetOwner();
    }
    if (!IsValid(PreviewActor) || !PreviewActor->GetWorld() ||
        PreviewActor->GetWorld()->WorldType != EWorldType::EditorPreview ||
        !IsValid(Blueprint->GeneratedClass) ||
        !PreviewActor->IsA(Blueprint->GeneratedClass)) return nullptr;

    // The generated Blueprint class has a real object property for each
    // Blueprint SCS component variable, bound to its current preview instance.
    // This remains reliable when USCS_Node::EditorComponentInstance is unset.
    const FName VariableName = Node->GetVariableName();
    if (const FObjectPropertyBase* ComponentProperty =
        FindFProperty<FObjectPropertyBase>(PreviewActor->GetClass(), VariableName))
    {
        USceneComponent* Component = Cast<USceneComponent>(
            ComponentProperty->GetObjectPropertyValue_InContainer(PreviewActor));
        if (IsValid(Component) && Component->GetOwner() == PreviewActor &&
            Component->IsA(Node->ComponentClass.Get())) return Component;
    }

    // SCS association remains a supported fallback, but only if it belongs to
    // this exact active preview Actor. Never trust a stale editor weak pointer.
    USceneComponent* Existing = Node->EditorComponentInstance.Get();
    if (IsValid(Existing) && Existing->GetOwner() == PreviewActor &&
        Existing->IsA(Node->ComponentClass.Get())) return Existing;
    return nullptr;
}

bool OriginBlueprintPivot::Resolve(UOriginPivotComponent* Pivot, UBlueprint*& OutBlueprint, USCS_Node*& OutNode)
{
    OutBlueprint = nullptr;
    OutNode = nullptr;
    if (!IsValid(Pivot) || !Pivot->IsTemplate()) return false;

    // The outer chain is not necessarily USCS_Node -> SCS -> Blueprint.
    // Engine-created SCS templates are often outered to the generated class.
    UBlueprint* Blueprint = FindOwningBlueprint(Pivot);
    if (!IsValid(Blueprint) || !IsValid(Blueprint->SimpleConstructionScript)) return false;
    USCS_Node* Node = FindAuthoredNode(Blueprint, Pivot, false);
    if (!IsValid(Node) || Node->ComponentTemplate.Get() != Pivot ||
        Node->GetSCS() != Blueprint->SimpleConstructionScript) return false;
    OutBlueprint = Blueprint;
    OutNode = Node;
    return true;
}

bool OriginBlueprintPivot::ResolveSelection(UOriginPivotComponent* Selected,
    UOriginPivotComponent*& OutTemplate, UBlueprint*& OutBlueprint, USCS_Node*& OutNode)
{
    OutTemplate = nullptr;
    OutBlueprint = nullptr;
    OutNode = nullptr;
    if (!IsValid(Selected)) return false;

    if (Resolve(Selected, OutBlueprint, OutNode))
    {
        OutTemplate = Selected;
        return true;
    }

    // The Blueprint Components panel sometimes customizes the preview instance
    // rather than the authored archetype. Map it only through the SCS editor's
    // explicit preview-component association, never by a variable name guess.
    if (Selected->IsTemplate() || !IsValid(Selected->GetOwner())) return false;
    AActor* Owner = Selected->GetOwner();
    if (!IsValid(Owner->GetWorld()) || Owner->GetWorld()->WorldType != EWorldType::EditorPreview)
        return false;
    UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Owner->GetClass());
    USCS_Node* Node = nullptr;
    if (IsValid(Blueprint) && IsValid(Blueprint->SimpleConstructionScript))
    {
        for (USCS_Node* Candidate : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (FindPreviewComponent(Blueprint, Candidate) == Selected)
            {
                Node = Candidate;
                break;
            }
        }
    }
    if (!IsValid(Node)) return false;
    UOriginPivotComponent* Template = Cast<UOriginPivotComponent>(Node->ComponentTemplate.Get());
    UBlueprint* VerifiedBlueprint = nullptr;
    USCS_Node* VerifiedNode = nullptr;
    if (!Resolve(Template, VerifiedBlueprint, VerifiedNode) ||
        VerifiedBlueprint != Blueprint || VerifiedNode != Node) return false;
    OutTemplate = Template;
    OutBlueprint = Blueprint;
    OutNode = Node;
    return true;
}

bool OriginBlueprintPivot::CompensateChild(const FTransform& BeforePivot,
    const FTransform& AfterPivot, const FTransform& BeforeChild, FTransform& OutAfterChild)
{
    OutAfterChild = BeforeChild;
    if (BeforePivot.ContainsNaN() || AfterPivot.ContainsNaN() || BeforeChild.ContainsNaN() ||
        !BeforePivot.IsValid() || !AfterPivot.IsValid() || !BeforeChild.IsValid() ||
        !ValidScale(BeforePivot.GetScale3D()) || !ValidScale(BeforeChild.GetScale3D()) ||
        !BeforePivot.GetScale3D().Equals(AfterPivot.GetScale3D(), TransformTolerance) ||
        !AfterPivot.GetRotation().IsNormalized()) return false;
    const FTransform InParentFrame = BeforeChild * BeforePivot;
    const FTransform Candidate = InParentFrame.GetRelativeTransform(AfterPivot);
    if (Candidate.ContainsNaN() || !Candidate.IsValid() ||
        !Candidate.GetScale3D().Equals(BeforeChild.GetScale3D(), 1.e-4) ||
        !(Candidate * AfterPivot).Equals(InParentFrame, 1.e-3)) return false;
    OutAfterChild = Candidate;
    return true;
}

bool OriginBlueprintPivot::Plan(UOriginPivotComponent* Pivot, const FTransform& NewRelative,
    FPlan& OutPlan, FText& OutReason)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Origin_BlueprintPivotPlan);
    ORIGIN_PERF_SCOPE(BlueprintPivotPlan);
    OutPlan = FPlan();
    UBlueprint* Blueprint = nullptr;
    USCS_Node* Node = nullptr;
    if (!Resolve(Pivot, Blueprint, Node))
    {
        OutReason = LOCTEXT("NotAuthored", "Select an Origin Pivot component authored in this Blueprint. Inherited and native component templates cannot be edited in this mode.");
        return false;
    }
    const FTransform Before = Pivot->GetRelativeTransform();
    if (USceneComponent* ParentTemplate = Node->GetParentComponentTemplate(Blueprint))
    {
        if (!ValidScale(ParentTemplate->GetRelativeScale3D()))
        {
            OutReason = LOCTEXT("UnsupportedAncestor", "The pivot's parent template uses a non-uniform or negative scale. Blueprint pivot editing is unavailable for this hierarchy.");
            return false;
        }
    }
    if (NewRelative.ContainsNaN() || !NewRelative.IsValid() ||
        !NewRelative.GetRotation().IsNormalized() ||
        !Before.GetScale3D().Equals(NewRelative.GetScale3D(), TransformTolerance) ||
        !ValidScale(Before.GetScale3D()))
    {
        OutReason = LOCTEXT("InvalidTransform", "Origin Blueprint pivots support only finite translation and rotation. Pivot scale must remain unchanged and uniformly positive.");
        return false;
    }
    OutPlan.BeforePivot = Before;
    OutPlan.AfterPivot = NewRelative;
    for (USCS_Node* ChildNode : Node->GetChildNodes())
    {
        if (!IsValid(ChildNode))
        {
            OutReason = LOCTEXT("InvalidChild", "An authored child component is missing. No changes were made.");
            return false;
        }
        USceneComponent* Child = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
        if (!IsValid(Child) || !ValidScale(Child->GetRelativeScale3D()))
        {
            OutReason = LOCTEXT("UnsupportedChild", "A direct child is missing a scene component template or uses non-uniform/negative scale. No changes were made.");
            return false;
        }
        FChildTransform& Entry = OutPlan.Children.AddDefaulted_GetRef();
        Entry.Node = ChildNode;
        Entry.Template = Child;
        Entry.Before = Child->GetRelativeTransform();
        // UE transform convention: ChildWorld = ChildRelative * ParentWorld.
        // Compute first, mutate only after all children are proven safe.
        if (!CompensateChild(Before, NewRelative, Entry.Before, Entry.After))
        {
            OutReason = LOCTEXT("CannotPreserve", "Unreal cannot preserve an attached child's full transform with this pivot change. No changes were made.");
            OutPlan = FPlan();
            return false;
        }
    }
    return true;
}


bool OriginBlueprintPivot::ReconcilePreviewChild(USceneComponent* PreviewPivot,
    USceneComponent* PreviewChild, const FTransform& InitialPivot,
    const FTransform& InitialChild)
{
    if (!IsValid(PreviewPivot) || !IsValid(PreviewChild) ||
        PreviewChild->GetAttachParent() != PreviewPivot) return false;
    FTransform Compensated;
    if (!CompensateChild(InitialPivot, PreviewPivot->GetRelativeTransform(),
            InitialChild, Compensated)) return false;
    SetTemplateRelative(PreviewChild, Compensated);
    return PreviewChild->GetRelativeTransform().Equals(Compensated, 1.e-3);
}

void OriginBlueprintPivot::Apply(UOriginPivotComponent* Pivot, const FPlan& Plan, bool bUseAfter)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Origin_BlueprintPivotApply);
    ORIGIN_PERF_SCOPE(BlueprintPivotApply);
    const FTransform& PivotRelative = bUseAfter ? Plan.AfterPivot : Plan.BeforePivot;
    SetTemplateRelative(Pivot, PivotRelative);
    for (const FChildTransform& Child : Plan.Children)
    {
        SetTemplateRelative(Child.Template.Get(), bUseAfter ? Child.After : Child.Before);
    }
    // The Blueprint viewport is a disposable preview. Keep it coherent during
    // the drag without saving preview actors/components into the transaction.
    UBlueprint* OwnerBlueprint = nullptr;
    USCS_Node* PivotNode = nullptr;
    Resolve(Pivot, OwnerBlueprint, PivotNode);
    if (PivotNode)
    {
        // Keep the active preview pivot available for child reconciliation.
        // Declaring it inside the conditional would leave it out of scope below.
        USceneComponent* Preview = FindPreviewComponent(OwnerBlueprint, PivotNode);
        if (Preview)
        {
            SetTemplateRelative(Preview, PivotRelative);
        }
        for (const FChildTransform& Child : Plan.Children)
        {
            if (USCS_Node* ChildNode = Child.Node.Get())
            {
                if (USceneComponent* PreviewChild = FindPreviewComponent(OwnerBlueprint, ChildNode))
                {
                    // Native Blueprint gizmos can move an entire preview
                    // attachment tree before Origin's editor mode receives the
                    // delta. Reconcile against the drag-start SCS baseline,
                    // not the already-moved child preview position.
                    if (Preview && !ReconcilePreviewChild(Preview, PreviewChild,
                            Plan.BeforePivot, Child.Before))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Origin: Preview child '%s' cannot be reconciled; authored child template was updated, preview requires rebuild."),
                            *ChildNode->GetVariableName().ToString());
                    }
                }
            }
        }
    }
}

FOriginBlueprintPivotEditSession::FOriginBlueprintPivotEditSession(UOriginPivotComponent* InTemplate, bool bInRecordUndo)
    : Template(InTemplate), bRecordUndo(bInRecordUndo)
{
    UBlueprint* BP = nullptr;
    USCS_Node* SCSNode = nullptr;
    if (OriginBlueprintPivot::Resolve(InTemplate, BP, SCSNode))
    {
        Blueprint = BP;
        Node = SCSNode;
        InitialPivot = InTemplate->GetRelativeTransform();
        for (USCS_Node* ChildNode : SCSNode->GetChildNodes())
        {
            if (USceneComponent* Child = ChildNode ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr)
            {
                OriginBlueprintPivot::FChildTransform& Saved = OriginalChildren.AddDefaulted_GetRef();
                Saved.Node = ChildNode;
                Saved.Template = Child;
                Saved.Before = Child->GetRelativeTransform();
            }
        }
    }
}

FOriginBlueprintPivotEditSession::~FOriginBlueprintPivotEditSession()
{
    if (bBegun) Cancel();
}

void FOriginBlueprintPivotEditSession::Begin(const OriginBlueprintPivot::FPlan& Plan)
{
    if (bBegun) return;
    bBegun = true;
    if (!bRecordUndo) return;
    Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("EditBlueprintPivot", "Origin: Edit Blueprint Pivot (Preserve Children)"));
    UBlueprint* BP = Blueprint.Get();
    USCS_Node* SCSNode = Node.Get();
    if (BP) BP->Modify();
    if (SCSNode)
    {
        if (USimpleConstructionScript* SCS = SCSNode->GetSCS()) SCS->Modify();
        SCSNode->Modify();
    }
    if (UOriginPivotComponent* Pivot = Template.Get()) Pivot->Modify();
    for (const OriginBlueprintPivot::FChildTransform& Child : Plan.Children)
    {
        if (USCS_Node* ChildNode = Child.Node.Get()) ChildNode->Modify();
        if (USceneComponent* ChildTemplate = Child.Template.Get()) ChildTemplate->Modify();
    }
}

void FOriginBlueprintPivotEditSession::PrepareForViewportDrag()
{
    if (!Template.IsValid() || bBegun) return;
    OriginBlueprintPivot::FPlan Snapshot;
    Snapshot.BeforePivot = InitialPivot;
    Snapshot.AfterPivot = InitialPivot;
    Snapshot.Children = OriginalChildren;
    Begin(Snapshot);
}

bool FOriginBlueprintPivotEditSession::NeedsReconciliation(const FTransform& PreviewPivotRelative) const
{
    const UOriginPivotComponent* Pivot = Template.Get();
    UBlueprint* BP = Blueprint.Get();
    USCS_Node* PivotNode = Node.Get();
    if (!IsValid(Pivot) || !IsValid(BP) || !IsValid(PivotNode) ||
        !PreviewPivotRelative.IsValid() || PreviewPivotRelative.ContainsNaN()) return false;

    // The editor may have already written the native drag into the source
    // template. Compare against the intended preview transform as well as
    // every original child's *unchanged* pivot-parent-space placement.
    if (!Pivot->GetRelativeTransform().Equals(PreviewPivotRelative, 1.e-4)) return true;
    // A viewport may have already written the first native delta into both
    // preview and template before we see it. Ensure it still enters our
    // transaction even when the pivot has no authored children.
    if (!bChanged && !InitialPivot.Equals(PreviewPivotRelative, 1.e-4)) return true;
    for (const OriginBlueprintPivot::FChildTransform& Original : OriginalChildren)
    {
        USceneComponent* ChildTemplate = Original.Template.Get();
        USCS_Node* ChildNode = Original.Node.Get();
        if (!IsValid(ChildTemplate) || !IsValid(ChildNode)) return false;
        FTransform Expected;
        if (!OriginBlueprintPivot::CompensateChild(InitialPivot, PreviewPivotRelative,
                Original.Before, Expected)) return false;
        if (!ChildTemplate->GetRelativeTransform().Equals(Expected, 1.e-4)) return true;
        if (USceneComponent* PreviewChild = OriginBlueprintPivot::FindPreviewComponent(BP, ChildNode))
        {
            if (!PreviewChild->GetRelativeTransform().Equals(Expected, 1.e-4)) return true;
        }
    }
    return false;
}

bool FOriginBlueprintPivotEditSession::Apply(const FTransform& NewRelative, FText& OutReason)
{
    UOriginPivotComponent* Pivot = Template.Get();
    OriginBlueprintPivot::FPlan Plan;
    if (!OriginBlueprintPivot::Plan(Pivot, NewRelative, Plan, OutReason))
    {
        if (bChanged) Cancel();
        return false;
    }
    // Blueprint viewport gizmos can update the selected SCS template AND move
    // its preview children before FEdMode::InputDelta receives the event.
    // Never use the already-moved children as the preservation baseline.
    if (Plan.Children.Num() != OriginalChildren.Num())
    {
        OutReason = LOCTEXT("HierarchyChangedDuringDrag", "The Blueprint's authored pivot hierarchy changed during the edit. Cancel and restart.");
        if (bChanged) Cancel();
        return false;
    }
    // Plan() observes the current template, which the Blueprint editor may
    // already have moved natively. Subsequent preview reconciliation must
    // instead use the immutable beginning-of-drag pivot frame.
    Plan.BeforePivot = InitialPivot;
    bool bRequiresApply = !Pivot->GetRelativeTransform().Equals(NewRelative, 1.e-5);
    for (OriginBlueprintPivot::FChildTransform& Child : Plan.Children)
    {
        const OriginBlueprintPivot::FChildTransform* Original = OriginalChildren.FindByPredicate(
            [&Child](const OriginBlueprintPivot::FChildTransform& Item)
            {
                return Item.Node == Child.Node && Item.Template == Child.Template;
            });
        if (!Original || !OriginBlueprintPivot::CompensateChild(
                InitialPivot, NewRelative, Original->Before, Child.After))
        {
            OutReason = LOCTEXT("SnapshotInvalid", "Cannot compensate the original child transform. No further pivot edit was applied.");
            if (bChanged) Cancel();
            return false;
        }
        Child.Before = Original->Before;
        if (USceneComponent* ChildTemplate = Child.Template.Get())
            bRequiresApply |= !ChildTemplate->GetRelativeTransform().Equals(Child.After, 1.e-5);
    }
    // Even if the native viewport has already moved the source component,
    // its preview children still need to be restored to the authored frame.
    bRequiresApply |= NeedsReconciliation(NewRelative);
    if (!bRequiresApply) return true;
    Begin(Plan);
    OriginBlueprintPivot::Apply(Pivot, Plan, true);
    bChanged = true;
    return true;
}

void FOriginBlueprintPivotEditSession::Finish()
{
    // Native viewport tooling may have modified the SCS source without going
    // through our InputDelta. If we never successfully reconciled it, discard
    // those changes instead of leaving an uncompensated hierarchy behind.
    if (bBegun && !bChanged && Template.IsValid())
    {
        bool bUnexpectedChange = !Template->GetRelativeTransform().Equals(InitialPivot, 1.e-4);
        for (const OriginBlueprintPivot::FChildTransform& Original : OriginalChildren)
        {
            if (USceneComponent* Child = Original.Template.Get())
                bUnexpectedChange |= !Child->GetRelativeTransform().Equals(Original.Before, 1.e-4);
        }
        if (bUnexpectedChange) { Cancel(); return; }
    }
    if (bChanged)
    {
        if (UBlueprint* BP = Blueprint.Get())
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
            BP->MarkPackageDirty();
        }
    }
    else if (Transaction) Transaction->Cancel();
    Transaction.Reset();
    bBegun = false;
    bChanged = false;
}

void FOriginBlueprintPivotEditSession::Cancel()
{
    if (bBegun && Template.IsValid())
    {
        OriginBlueprintPivot::FPlan Original;
        Original.AfterPivot = InitialPivot;
        Original.BeforePivot = InitialPivot;
        Original.Children = OriginalChildren;
        for (OriginBlueprintPivot::FChildTransform& Child : Original.Children) Child.After = Child.Before;
        OriginBlueprintPivot::Apply(Template.Get(), Original, true);
    }
    if (Transaction) Transaction->Cancel();
    Transaction.Reset();
    bBegun = false;
    bChanged = false;
}
#undef LOCTEXT_NAMESPACE
