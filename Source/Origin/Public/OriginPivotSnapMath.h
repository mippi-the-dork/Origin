// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "OriginPivotTypes.h"

/**
 * Scene-independent inputs for an Origin pivot source calculation.
 * All locations and bounds MUST be expressed in one consistent frame:
 * world space for placed Actors/components, the authored parent frame for
 * Blueprint SCS component templates. The host adapter is responsible for
 * verifying reference ownership, attachment relationships and socket validity.
 */
struct ORIGIN_API FOriginPivotCalculationInput
{
    FBox AssemblyBounds = FBox(ForceInit);
    TArray<FVector> DirectChildOrigins;
    FVector ReferencePosition = FVector::ZeroVector;
    bool bHasReferencePosition = false;
    FQuat PivotRotation = FQuat::Identity;
};

/** Pure shared calculations; contains no Editor modules, asset loads or side effects. */
struct ORIGIN_API FOriginPivotSnapMath
{
    /** Arithmetic mean of direct child origins in the pivot's parent frame. */
    static bool AverageChildOrigins(const FTransform& PivotInParent,
        const TArray<FTransform>& ChildrenInPivot, FVector& OutCenter);

    /**
     * Compute a location from the same serialized definition on either host.
     * Never modifies an Actor, component, slot or world. Invalid/unavailable
     * sources fail without replacing the position with world origin.
     */
    static bool CalculatePosition(const FOriginPivotDefinition& Definition,
        const FOriginPivotCalculationInput& Input, FVector& OutPosition, FString& OutError);
};
