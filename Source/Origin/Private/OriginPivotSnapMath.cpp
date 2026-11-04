// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginPivotSnapMath.h"

bool FOriginPivotSnapMath::AverageChildOrigins(const FTransform& PivotInParent,
    const TArray<FTransform>& ChildrenInPivot, FVector& OutCenter)
{
    if (ChildrenInPivot.IsEmpty() || !PivotInParent.IsValid() || PivotInParent.ContainsNaN())
        return false;
    FVector Sum = FVector::ZeroVector;
    for (const FTransform& Child : ChildrenInPivot)
    {
        if (!Child.IsValid() || Child.ContainsNaN()) return false;
        const FTransform InParent = Child * PivotInParent;
        if (!InParent.IsValid() || InParent.ContainsNaN()) return false;
        Sum += InParent.GetTranslation();
    }
    OutCenter = Sum / static_cast<double>(ChildrenInPivot.Num());
    return !OutCenter.ContainsNaN();
}
