// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginPivotSnapMath.h"

bool FOriginPivotSnapMath::AverageChildOrigins(const FTransform& PivotInParent,
    const TArray<FTransform>& ChildrenInPivot, FVector& OutCenter)
{
    if (ChildrenInPivot.IsEmpty() || !PivotInParent.IsValid() || PivotInParent.ContainsNaN())
        return false;
    FOriginPivotCalculationInput Input;
    Input.PivotRotation = PivotInParent.GetRotation();
    for (const FTransform& Child : ChildrenInPivot)
    {
        if (!Child.IsValid() || Child.ContainsNaN()) return false;
        const FTransform InParent = Child * PivotInParent;
        if (!InParent.IsValid() || InParent.ContainsNaN()) return false;
        Input.DirectChildOrigins.Add(InParent.GetTranslation());
    }
    FOriginPivotDefinition Definition;
    Definition.PositionMode = EOriginPivotMode::AverageChildPivots;
    FString Error;
    return CalculatePosition(Definition, Input, OutCenter, Error);
}

bool FOriginPivotSnapMath::CalculatePosition(const FOriginPivotDefinition& Definition,
    const FOriginPivotCalculationInput& Input, FVector& OutPosition, FString& OutError)
{
    OutError.Empty();
    auto Fail = [&OutError](const TCHAR* Message)
    {
        OutError = Message;
        return false;
    };
    // Reject invalid values before reading a source. A failed calculation must
    // NEVER silently default to (0,0,0), even for invalid named targets.
    if (Definition.Offset.ContainsNaN() || Definition.CustomPosition.ContainsNaN() ||
        Input.PivotRotation.ContainsNaN() || !Input.PivotRotation.IsNormalized())
        return Fail(TEXT("The pivot calculation contains an invalid coordinate or rotation."));

    FVector Candidate = FVector::ZeroVector;
    switch (Definition.PositionMode)
    {
    case EOriginPivotMode::BoundsCenter:
    case EOriginPivotMode::BottomCenter:
    case EOriginPivotMode::BoundsPoint:
    {
        if (!Input.AssemblyBounds.IsValid ||
            Input.AssemblyBounds.Min.ContainsNaN() || Input.AssemblyBounds.Max.ContainsNaN())
            return Fail(TEXT("The assembly has no valid geometry bounds or child positions."));
        Candidate = Input.AssemblyBounds.GetCenter();
        if (Definition.PositionMode == EOriginPivotMode::BottomCenter)
            Candidate.Z = Input.AssemblyBounds.Min.Z;
        if (Definition.PositionMode == EOriginPivotMode::BoundsPoint)
        {
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                const int32 Selection = Definition.BoundsAxes[Axis];
                if (Selection < 0 || Selection > 2)
                    return Fail(TEXT("Choose Min, Center or Max for each bounds axis."));
                if (Selection == 0) Candidate[Axis] = Input.AssemblyBounds.Min[Axis];
                if (Selection == 2) Candidate[Axis] = Input.AssemblyBounds.Max[Axis];
            }
        }
        break;
    }
    case EOriginPivotMode::AverageChildPivots:
    {
        if (Input.DirectChildOrigins.IsEmpty())
            return Fail(TEXT("Add a direct child before averaging child origins."));
        for (const FVector& Location : Input.DirectChildOrigins)
        {
            if (Location.ContainsNaN())
                return Fail(TEXT("A direct child has an invalid origin."));
            Candidate += Location;
        }
        Candidate /= static_cast<double>(Input.DirectChildOrigins.Num());
        break;
    }
    case EOriginPivotMode::ChildActor:
    case EOriginPivotMode::ComponentSocket:
    case EOriginPivotMode::ReferenceActor:
        if (!Input.bHasReferencePosition || Input.ReferencePosition.ContainsNaN())
            return Fail(TEXT("The chosen pivot source is unavailable or has an invalid position."));
        Candidate = Input.ReferencePosition;
        break;
    case EOriginPivotMode::WorldOrigin:
        Candidate = FVector::ZeroVector;
        break;
    case EOriginPivotMode::Custom:
        Candidate = Definition.CustomPosition;
        break;
    default:
        return Fail(TEXT("Choose a supported pivot source mode."));
    }

    Candidate += Definition.bLocalOffset
        ? Input.PivotRotation.RotateVector(Definition.Offset) : Definition.Offset;
    if (Candidate.ContainsNaN())
        return Fail(TEXT("The calculated pivot position is invalid."));
    OutPosition = Candidate; // Assignment only on success; caller may be holding previous value.
    return true;
}
