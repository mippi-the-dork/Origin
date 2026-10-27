// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "OriginPivotCore.h"
#include "OriginPivotTypes.h"
#include "OriginPivotComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOriginPivotChangedSignature,
    FTransform, PreviousPivotWorldTransform, FTransform, NewPivotWorldTransform);

/**
 * An authorable pivot for Blueprint component hierarchies and attached Actors.
 * Children are attached using standard Unreal SceneComponent attachment rules.
 * Ordinary movement uses standard SceneComponent transform nodes; special pivot
 * editing keeps the attached assembly stationary instead of moving it.
 *
 * Version 2.0 alpha 1: explicit pivot edits only; slots, migration, and replicated
 * pivot-authoring state will be introduced in later, separately tested phases.
 */
UCLASS(ClassGroup=(Origin), BlueprintType, Blueprintable,
    meta=(BlueprintSpawnableComponent, DisplayName="Origin Pivot"))
class ORIGIN_API UOriginPivotComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UOriginPivotComponent();

    /** Shared runtime data shape; slot authoring/activation is not enabled in Alpha 2. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot|Slots")
    FOriginPivotConfiguration OriginPivotConfiguration;

    /** Fires only for successful Origin pivot edits, not ordinary SceneComponent movement. */
    UPROPERTY(BlueprintAssignable, Category="Origin|Pivot")
    FOriginPivotChangedSignature OnOriginPivotChanged;

    /**
     * Change this pivot's world location/rotation while leaving its attached
     * components and attached Actors at their existing world transforms.
     * The input scale must match the existing pivot scale.
     * Returns NoChange if the requested transform already matches.
     */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot",
        meta=(DisplayName="Set Origin Pivot (Preserve Children)"))
    EOriginPivotResult SetOriginPivotWorldTransform(
        const FTransform& NewPivotWorldTransform,
        FText& FailureReason);

    /** Check a proposed pivot edit without modifying transforms. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot",
        meta=(DisplayName="Can Set Origin Pivot"))
    EOriginPivotResult CanSetOriginPivotWorldTransform(
        const FTransform& NewPivotWorldTransform,
        FText& FailureReason) const;
};
