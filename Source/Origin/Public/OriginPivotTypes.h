// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OriginPivotTypes.generated.h"

class AActor;

/**
 * Existing Origin 1.x position modes. Never renumber these serialized enum entries.
 * Available in the runtime module so authored definitions survive cooking.
 */
UENUM(BlueprintType)
enum class EOriginPivotMode : uint8
{
    BoundsCenter UMETA(DisplayName="Bounds Center"),
    AverageChildPivots UMETA(DisplayName="Average Child Pivots"),
    BottomCenter UMETA(DisplayName="Bottom Center"),
    ChildActor UMETA(DisplayName="Child Actor"),
    WorldOrigin UMETA(DisplayName="World Origin"),
    Custom UMETA(DisplayName="Custom"),
    BoundsPoint UMETA(DisplayName="Bounds Point"),
    ReferenceActor UMETA(DisplayName="Reference Actor"),
    ComponentSocket UMETA(DisplayName="Component / Socket")
};

/**
 * The serialized source/settings for an Origin pivot. Alpha 2 copies the existing
 * 1.x settings exactly; these are definitions, NOT instructions to recalculate
 * an already placed assembly on load. Orientation/slot execution comes later.
 */
USTRUCT(BlueprintType)
struct ORIGIN_API FOriginPivotDefinition
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    EOriginPivotMode PositionMode = EOriginPivotMode::BoundsCenter;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    TSoftObjectPtr<AActor> ChildActor;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FVector CustomPosition = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FIntVector BoundsAxes = FIntVector(1, 1, 1);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    TSoftObjectPtr<AActor> ReferenceActor;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FName ComponentName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FName SocketName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FVector Offset = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    bool bLocalOffset = false;
};

/**
 * Named authoring pose in the component parent's local space.
 * Existing v1/Alpha 2 slots have bHasSavedRelativeTransform=false and remain
 * untouched until the user explicitly captures a pose. Never guess a migrated
 * slot's pose or reposition content during PostLoad.
 */
USTRUCT(BlueprintType)
struct ORIGIN_API FOriginPivotSlot
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FName Name = TEXT("Default");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FOriginPivotDefinition Definition;

    /** Whether this slot has an explicitly captured transform (legacy slots do not). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    bool bHasSavedRelativeTransform = false;

    /** Pivot pose relative to its immediate attachment parent, not world space. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FTransform SavedRelativeTransform = FTransform::Identity;

    /** True when an unattached Anchor captured this pose against the level world frame. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    bool bCapturedAtWorldRoot = false;

    /** Socket frame used by an explicitly captured pose. NAME_None means component origin. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FName CapturedSocketName = NAME_None;
};

/**
 * Versioned, runtime-serializable pivot data shared by Anchors and Components.
 * Legacy definitions remain unmodified; named local-space transform snapshots
 * can be explicitly saved and activated without modifying the entire assembly.
 */
USTRUCT(BlueprintType)
struct ORIGIN_API FOriginPivotConfiguration
{
    GENERATED_BODY()

    // 0: legacy source; 1: position definitions imported; 2: local-pose slots available.
    UPROPERTY()
    int32 SchemaVersion = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    FName ActiveSlotName = TEXT("Default");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Origin|Pivot")
    TArray<FOriginPivotSlot> Slots;

    const FOriginPivotSlot* FindSlot(FName Name) const
    {
        return Slots.FindByPredicate([Name](const FOriginPivotSlot& Slot) { return Slot.Name == Name; });
    }

    FOriginPivotSlot* FindSlot(FName Name)
    {
        return Slots.FindByPredicate([Name](const FOriginPivotSlot& Slot) { return Slot.Name == Name; });
    }
};
