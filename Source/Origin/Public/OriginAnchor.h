#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OriginAnchor.generated.h"

UENUM()
enum class EOriginPivotMode : uint8
{
    BoundsCenter UMETA(DisplayName="Bounds Center"),
    AverageChildPivots UMETA(DisplayName="Average Child Pivots"),
    BottomCenter UMETA(DisplayName="Bottom Center"),
    ChildActor UMETA(DisplayName="Child Actor"),
    WorldOrigin UMETA(DisplayName="World Origin"),
    Custom UMETA(DisplayName="Custom"),
    // Append modes so existing levels retain their serialized enum values.
    BoundsPoint UMETA(DisplayName="Bounds Point"),
    ReferenceActor UMETA(DisplayName="Reference Actor"),
    ComponentSocket UMETA(DisplayName="Component / Socket")
};

/** A runtime transform parent. Pivot calculation and all UI live in OriginEditor. */
UCLASS(NotBlueprintable, ClassGroup=(Origin), meta=(DisplayName="Origin Anchor"))
class ORIGIN_API AOriginAnchor : public AActor
{
    GENERATED_BODY()
public:
    AOriginAnchor();
#if WITH_EDITORONLY_DATA
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
