// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OriginPivotTypes.h"
#include "OriginPivotCore.h"
#include "OriginAnchor.generated.h"

/** A runtime transform parent. Pivot calculation and all UI live in OriginEditor. */
UCLASS(NotBlueprintable, ClassGroup=(Origin), meta=(DisplayName="Origin Anchor"))
class ORIGIN_API AOriginAnchor : public AActor
{
    GENERATED_BODY()
public:
    AOriginAnchor();

    virtual void PostLoad() override;
    virtual void PostActorCreated() override;

    /** Read a migrated, runtime-safe pivot definition (does not move the Actor). */
    UFUNCTION(BlueprintPure, Category="Origin|Pivot|Slots", meta=(DisplayName="Get Origin Pivot Slot Definition"))
    bool GetOriginPivotSlotDefinition(FName SlotName, FOriginPivotDefinition& OutDefinition) const;

    /** Anchor and component slots use the same serialized data and naming rules. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Create Origin Pivot Slot"))
    bool CreateOriginPivotSlot(FName SlotName, FText& FailureReason);
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Capture Origin Pivot Slot"))
    bool CaptureOriginPivotSlot(FName SlotName, FText& FailureReason);
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Duplicate Origin Pivot Slot"))
    bool DuplicateOriginPivotSlot(FName SourceName, FName NewName, FText& FailureReason);
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Rename Origin Pivot Slot"))
    bool RenameOriginPivotSlot(FName OldName, FName NewName, FText& FailureReason);
    UFUNCTION(BlueprintPure, Category="Origin|Pivot|Slots", meta=(DisplayName="Get Origin Pivot Slot Names"))
    void GetOriginPivotSlotNames(TArray<FName>& OutNames) const;
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Delete Origin Pivot Slot"))
    bool DeleteOriginPivotSlot(FName SlotName, FText& FailureReason);
    /** Root-level Anchor captures are stored in world space; attached Anchors use the parent component's frame. */
    UFUNCTION(BlueprintPure, Category="Origin|Pivot|Slots", meta=(DisplayName="Get Origin Pivot Slot Pose"))
    bool GetOriginPivotSlotPose(FName SlotName, FTransform& OutRelativePose) const;
    /** Requires a Movable, registered, non-physics assembly. Does not change attached geometry. */
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot|Slots", meta=(DisplayName="Activate Origin Pivot Slot"))
    EOriginPivotResult ActivateOriginPivotSlot(FName SlotName, FText& FailureReason);

    UPROPERTY(BlueprintAssignable, Category="Origin|Pivot")
    FOriginPivotChangedSignature OnOriginPivotChanged;

    UFUNCTION(BlueprintCallable, Category="Origin|Pivot", meta=(DisplayName="Set Origin Pivot (Preserve Children)"))
    EOriginPivotResult SetOriginPivotWorldTransform(const FTransform& NewPivotWorldTransform, FText& FailureReason);
    UFUNCTION(BlueprintCallable, Category="Origin|Pivot", meta=(DisplayName="Can Set Origin Pivot"))
    EOriginPivotResult CanSetOriginPivotWorldTransform(const FTransform& NewPivotWorldTransform, FText& FailureReason) const;

    /** Runtime-serializable named slot data shared with Origin Pivot components. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot|Migration")
    FOriginPivotConfiguration OriginPivotConfiguration;

#if WITH_EDITOR
    /** Synchronize the legacy editor-authoring fields into the Default runtime slot. */
    void SyncLegacyPivotToRuntimeDefinition();
#endif
#if WITH_EDITOR
    virtual void PostRegisterAllComponents() override;
    virtual void PostEditUndo() override;
    void RefreshEditorBillboard();
    bool IsOriginEditorComponent(const UActorComponent* Component) const;
#endif
#if WITH_EDITORONLY_DATA
    // Editor visualization only. Neither this component nor its visibility
    // preference is needed in a packaged game.
    UPROPERTY(Transient)
    TObjectPtr<class UBillboardComponent> EditorBillboard;
    UPROPERTY()
    bool bShowEditorBillboard = true;
    // Custom Details controls commit these together with a pivot move in one transaction.
    UPROPERTY()
    EOriginPivotMode PivotMode = EOriginPivotMode::BoundsCenter;
    UPROPERTY()
    TSoftObjectPtr<AActor> PivotChild;
    UPROPERTY()
    FVector CustomPivot = FVector::ZeroVector;
    // Axis values: 0 = minimum, 1 = center, 2 = maximum (world-space bounds).
    UPROPERTY()
    FIntVector BoundsAxes = FIntVector(1, 1, 1);
    UPROPERTY()
    TSoftObjectPtr<AActor> ReferenceActor;
    // Component names survive normal Blueprint component reconstruction.
    UPROPERTY()
    FName PivotComponentName;
    UPROPERTY()
    FName PivotSocketName;
    UPROPERTY()
    FVector OriginPivotOffset = FVector::ZeroVector;
    // Local offsets use anchor rotation, not scale, and remain measured in cm.
    UPROPERTY()
    bool bLocalPivotOffset = false;
#endif
};
