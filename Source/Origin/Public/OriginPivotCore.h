// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OriginPivotCore.generated.h"

class USceneComponent;

/** The result of a pivot edit. A successful NoChange does not broadcast a change event. */
UENUM(BlueprintType)
enum class EOriginPivotResult : uint8
{
    Success UMETA(DisplayName="Success"),
    NoChange UMETA(DisplayName="No Change"),
    InvalidPivot UMETA(DisplayName="Invalid Pivot"),
    InvalidTransform UMETA(DisplayName="Invalid Transform"),
    UnsupportedScale UMETA(DisplayName="Unsupported Scale"),
    UnsupportedMobility UMETA(DisplayName="Unsupported Mobility"),
    SimulatingPhysics UMETA(DisplayName="Simulating Physics"),
    UnregisteredComponent UMETA(DisplayName="Unregistered Component"),
    PreservationFailed UMETA(DisplayName="Preservation Failed")
};

/**
 * Runtime-only pivot operations shared by Origin Anchor and Origin Pivot Component.
 * No editor module, editor-world assumption, actor replacement, or Tick is required.
 *
 * A pivot edit moves the pivot scene component while restoring the world transforms
 * of its directly attached components. Native attachment propagation keeps their
 * descendants stationary. This includes root components belonging to attached Actors.
 *
 * The initial supported profile is registered, movable, kinematic hierarchies with
 * positive uniform scale. Unsupported cases are rejected before any mutation.
 */
struct ORIGIN_API FOriginPivotCore
{
    static EOriginPivotResult ValidatePivotEdit(
        const USceneComponent* Pivot,
        const FTransform& NewPivotWorldTransform,
        FText& OutReason);

    static EOriginPivotResult SetPivotWorldTransformPreservingChildren(
        USceneComponent* Pivot,
        const FTransform& NewPivotWorldTransform,
        FText& OutReason);
};
