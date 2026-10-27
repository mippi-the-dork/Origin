// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotCore.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"

#define LOCTEXT_NAMESPACE "OriginPivotCore"

namespace
{
    struct FComponentWorldSnapshot
    {
        TWeakObjectPtr<USceneComponent> Component;
        FTransform WorldTransform;
    };

    EOriginPivotResult Reject(EOriginPivotResult Result, const FText& Reason, FText& OutReason)
    {
        OutReason = Reason;
        return Result;
    }

    bool HasSupportedScale(const FVector& Scale)
    {
        constexpr double Tolerance = 1.e-4;
        return Scale.X > Tolerance && Scale.Y > Tolerance && Scale.Z > Tolerance
            && FMath::IsNearlyEqual(Scale.X, Scale.Y, Tolerance)
            && FMath::IsNearlyEqual(Scale.Y, Scale.Z, Tolerance);
    }

    void GatherDescendants(const USceneComponent* Pivot, TArray<USceneComponent*>& OutDescendants)
    {
        Pivot->GetChildrenComponents(true, OutDescendants);
    }

    bool MatchesWorldTransform(const FTransform& Expected, const FTransform& Actual)
    {
        // Unreal transforms are float-backed or double-backed depending on build.
        // Use centimetre-scale tolerance rather than exact floating-point equality.
        return Expected.GetLocation().Equals(Actual.GetLocation(), 0.01)
            && Expected.GetRotation().Equals(Actual.GetRotation(), 1.e-4)
            && Expected.GetScale3D().Equals(Actual.GetScale3D(), 1.e-4);
    }
}

EOriginPivotResult FOriginPivotCore::ValidatePivotEdit(
    const USceneComponent* Pivot,
    const FTransform& NewPivotWorldTransform,
    FText& OutReason)
{
    OutReason = FText::GetEmpty();
    if (!IsValid(Pivot) || Pivot->IsTemplate() || !IsValid(Pivot->GetOwner()))
    {
        return Reject(EOriginPivotResult::InvalidPivot,
            LOCTEXT("InvalidPivot", "Choose a live Origin pivot component belonging to an Actor."), OutReason);
    }

    if (NewPivotWorldTransform.ContainsNaN() || !NewPivotWorldTransform.GetRotation().IsNormalized())
    {
        return Reject(EOriginPivotResult::InvalidTransform,
            LOCTEXT("InvalidTransform", "The requested pivot transform must be finite and its rotation normalized."), OutReason);
    }

    if (!Pivot->IsRegistered())
    {
        return Reject(EOriginPivotResult::UnregisteredComponent,
            LOCTEXT("UnregisteredPivot", "The pivot must be registered before its world transform can be edited."), OutReason);
    }

    const FTransform Current = Pivot->GetComponentTransform();
    if (Current.ContainsNaN())
    {
        return Reject(EOriginPivotResult::InvalidTransform,
            LOCTEXT("InvalidCurrent", "The pivot's current world transform is invalid."), OutReason);
    }
    if (!NewPivotWorldTransform.GetScale3D().Equals(Current.GetScale3D(), 1.e-4))
    {
        return Reject(EOriginPivotResult::UnsupportedScale,
            LOCTEXT("ScaleChange", "Editing a pivot cannot change its scale. Move or rotate the pivot only."), OutReason);
    }
    if (!HasSupportedScale(Current.GetScale3D()))
    {
        return Reject(EOriginPivotResult::UnsupportedScale,
            LOCTEXT("PivotScale", "Origin's initial runtime pivot editing requires positive, uniform scale."), OutReason);
    }

    if (Pivot->Mobility != EComponentMobility::Movable)
    {
        return Reject(EOriginPivotResult::UnsupportedMobility,
            LOCTEXT("StaticPivot", "Make the pivot Movable before editing its runtime transform."), OutReason);
    }

    for (const USceneComponent* Ancestor = Pivot->GetAttachParent(); Ancestor; Ancestor = Ancestor->GetAttachParent())
    {
        if (!HasSupportedScale(Ancestor->GetComponentTransform().GetScale3D()))
        {
            return Reject(EOriginPivotResult::UnsupportedScale,
                LOCTEXT("AncestorScale", "An attachment ancestor has negative, zero, or non-uniform scale."), OutReason);
        }
        if (const UPrimitiveComponent* PrimitiveAncestor = Cast<UPrimitiveComponent>(Ancestor))
        {
            if (PrimitiveAncestor->IsSimulatingPhysics())
            {
                return Reject(EOriginPivotResult::SimulatingPhysics,
                    LOCTEXT("PhysicsAncestor", "An attachment ancestor is simulating physics. Disable simulation before editing this pivot."), OutReason);
            }
        }
    }

    if (MatchesWorldTransform(Current, NewPivotWorldTransform))
    {
        return EOriginPivotResult::NoChange;
    }

    TArray<USceneComponent*> Descendants;
    GatherDescendants(Pivot, Descendants);
    for (const USceneComponent* Child : Descendants)
    {
        if (!Child->IsRegistered())
        {
            return Reject(EOriginPivotResult::UnregisteredComponent,
                LOCTEXT("UnregisteredChild", "An attached component is not registered. Finish registration before changing the pivot."), OutReason);
        }
        if (Child->Mobility != EComponentMobility::Movable)
        {
            return Reject(EOriginPivotResult::UnsupportedMobility,
                LOCTEXT("StaticChild", "An attached component is Static or Stationary. Make the moving assembly Movable."), OutReason);
        }
        if (Child->GetComponentTransform().ContainsNaN() || !HasSupportedScale(Child->GetComponentTransform().GetScale3D()))
        {
            return Reject(EOriginPivotResult::UnsupportedScale,
                LOCTEXT("ChildScale", "An attached component has an unsupported scale or transform."), OutReason);
        }
        if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child))
        {
            if (Primitive->IsSimulatingPhysics())
            {
                return Reject(EOriginPivotResult::SimulatingPhysics,
                    LOCTEXT("PhysicsChild", "A component in this assembly is simulating physics. Disable simulation before editing its pivot."), OutReason);
            }
        }
    }
    return EOriginPivotResult::Success;
}

EOriginPivotResult FOriginPivotCore::SetPivotWorldTransformPreservingChildren(
    USceneComponent* Pivot,
    const FTransform& NewPivotWorldTransform,
    FText& OutReason)
{
    const EOriginPivotResult Validation = ValidatePivotEdit(Pivot, NewPivotWorldTransform, OutReason);
    if (Validation != EOriginPivotResult::Success) return Validation;

    const FTransform OriginalPivot = Pivot->GetComponentTransform();
    TArray<USceneComponent*> DirectAttachedComponents;
    Pivot->GetChildrenComponents(false, DirectAttachedComponents);
    TArray<FComponentWorldSnapshot> DirectChildren;
    DirectChildren.Reserve(DirectAttachedComponents.Num());
    for (USceneComponent* Child : DirectAttachedComponents)
    {
        if (IsValid(Child))
        {
            FComponentWorldSnapshot Snapshot;
            Snapshot.Component = Child;
            Snapshot.WorldTransform = Child->GetComponentTransform();
            DirectChildren.Add(MoveTemp(Snapshot));
        }
    }

    // Snapshots of ALL descendants catch transform loss deeper in the hierarchy,
    // including independent child Actors attached through their root component.
    TArray<USceneComponent*> Descendants;
    GatherDescendants(Pivot, Descendants);
    TArray<FComponentWorldSnapshot> DescendantSnapshots;
    DescendantSnapshots.Reserve(Descendants.Num());
    for (USceneComponent* Child : Descendants)
    {
        FComponentWorldSnapshot Snapshot;
        Snapshot.Component = Child;
        Snapshot.WorldTransform = Child->GetComponentTransform();
        DescendantSnapshots.Add(MoveTemp(Snapshot));
    }

    Pivot->SetWorldTransform(NewPivotWorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
    for (const FComponentWorldSnapshot& Child : DirectChildren)
    {
        if (USceneComponent* Component = Child.Component.Get())
        {
            Component->SetWorldTransform(Child.WorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
        }
    }

    bool bPreserved = MatchesWorldTransform(NewPivotWorldTransform, Pivot->GetComponentTransform());
    for (const FComponentWorldSnapshot& Descendant : DescendantSnapshots)
    {
        const USceneComponent* Component = Descendant.Component.Get();
        bPreserved &= Component && MatchesWorldTransform(Descendant.WorldTransform, Component->GetComponentTransform());
    }
    if (!bPreserved)
    {
        // Restore using the original parent frame and each direct child's exact
        // captured world transform. Never leave an unsuccessful operation halfway.
        Pivot->SetWorldTransform(OriginalPivot, false, nullptr, ETeleportType::TeleportPhysics);
        for (const FComponentWorldSnapshot& Child : DirectChildren)
        {
            if (USceneComponent* Component = Child.Component.Get())
            {
                Component->SetWorldTransform(Child.WorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
            }
        }
        return Reject(EOriginPivotResult::PreservationFailed,
            LOCTEXT("PreservationFailed", "The requested pivot edit could not preserve all child world transforms; the previous pivot was restored."), OutReason);
    }

    OutReason = FText::GetEmpty();
    return EOriginPivotResult::Success;
}

#undef LOCTEXT_NAMESPACE
