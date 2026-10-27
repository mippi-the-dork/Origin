// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OriginPivotTypes.h"
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

    /** Read only in Alpha 2. Named slot editing and switching are later phases. */
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
