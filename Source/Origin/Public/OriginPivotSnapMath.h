// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Stateless snap math; scene/Blueprint adapters supply transforms in a shared frame. */
struct ORIGIN_API FOriginPivotSnapMath
{
    /** Returns the arithmetic mean of direct-child origins in the pivot's parent frame. */
    static bool AverageChildOrigins(const FTransform& PivotInParent,
        const TArray<FTransform>& ChildrenInPivot, FVector& OutCenter);
};
