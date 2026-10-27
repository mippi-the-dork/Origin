// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponent.h"

UOriginPivotComponent::UOriginPivotComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetMobility(EComponentMobility::Movable);
    OriginPivotConfiguration.SchemaVersion = 1;
    OriginPivotConfiguration.Slots.AddDefaulted();
}

EOriginPivotResult UOriginPivotComponent::CanSetOriginPivotWorldTransform(
    const FTransform& NewPivotWorldTransform, FText& FailureReason) const
{
    return FOriginPivotCore::ValidatePivotEdit(this, NewPivotWorldTransform, FailureReason);
}

EOriginPivotResult UOriginPivotComponent::SetOriginPivotWorldTransform(
    const FTransform& NewPivotWorldTransform, FText& FailureReason)
{
    const FTransform Before = GetComponentTransform();
    const EOriginPivotResult Result = FOriginPivotCore::SetPivotWorldTransformPreservingChildren(
        this, NewPivotWorldTransform, FailureReason);
    if (Result == EOriginPivotResult::Success)
    {
        OnOriginPivotChanged.Broadcast(Before, GetComponentTransform());
    }
    return Result;
}
