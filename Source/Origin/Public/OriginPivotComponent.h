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
 * Slot poses are stored in parent-relative coordinates. Normal scene-component
 * movement never writes slots; use Capture to explicitly save an authored pose.
 */
UCLASS(ClassGroup=(Origin), BlueprintType, Blueprintable,
    meta=(BlueprintSpawnableComponent, DisplayName="Origin Pivot"))
class ORIGIN_API UOriginPivotComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UOriginPivotComponent();

    /** Persistent named pivot definitions and captured parent-relative poses. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot|Slots")
    FOriginPivotConfiguration OriginPivotConfiguration;

    /** Read a stored pivot definition without changing the component transform. */
    UFUNCTION(BlueprintPure, Category="Origin|Pivot|Slots", meta=(DisplayName="Get Origin Pivot Slot Definition"))
    bool GetOriginPivotSlotDefinition(FName SlotName, FOriginPivotDefinition& OutDefinition) const;

    /** Save the current parent-relative pivot pose to a new named slot. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Create Origin Pivot Slot"))
    bool CreateOriginPivotSlot(FName SlotName, FText& FailureReason);

    /** Overwrite an existing slot with the current parent-relative pivot pose. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Capture Origin Pivot Slot"))
    bool CaptureOriginPivotSlot(FName SlotName, FText& FailureReason);

    /** Duplicate an existing slot definition and pose under a new name (without activating it). */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Duplicate Origin Pivot Slot"))
    bool DuplicateOriginPivotSlot(FName SourceName, FName NewName, FText& FailureReason);

    /** Rename a non-Default slot, updating ActiveSlotName if necessary. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Rename Origin Pivot Slot"))
    bool RenameOriginPivotSlot(FName OldName, FName NewName, FText& FailureReason);

    UFUNCTION(BlueprintPure, Category="Origin|Pivot|Slots", meta=(DisplayName="Get Origin Pivot Slot Names"))
    void GetOriginPivotSlotNames(TArray<FName>& OutNames) const;

    /** Delete an inactive slot. The required Default slot cannot be deleted. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Delete Origin Pivot Slot"))
    bool DeleteOriginPivotSlot(FName SlotName, FText& FailureReason);

    /** Query an explicitly captured slot pose (including whether it exists). */
    UFUNCTION(BlueprintPure, Category="Origin|Pivot|Slots", meta=(DisplayName="Get Origin Pivot Slot Pose"))
    bool GetOriginPivotSlotPose(FName SlotName, FTransform& OutRelativePose) const;

    /**
     * Switch to a captured slot while preserving the attached assembly in world
     * space. Runtime instances only; use the editor's authored Blueprint action
     * for Blueprint component templates. Invalid attempts change nothing.
     */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Activate Origin Pivot Slot"))
    EOriginPivotResult ActivateOriginPivotSlot(FName SlotName, FText& FailureReason);

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
