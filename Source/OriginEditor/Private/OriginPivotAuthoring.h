// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UOriginPivotComponent;

enum class EOriginPivotSnapAction : uint8
{
    AverageDirectChildren,
    ParentOrigin,
    ParentOrientation
};

/** Editor-only, transaction-safe slot authoring and conservative pivot snapping. */
struct FOriginPivotAuthoring
{
    static bool CanAuthor(UOriginPivotComponent* Pivot);
    static bool CreateSlot(UOriginPivotComponent* Pivot, FName Name, FText& OutReason);
    static bool CaptureSlot(UOriginPivotComponent* Pivot, FName Name, FText& OutReason);
    static bool RemoveSlot(UOriginPivotComponent* Pivot, FName Name, FText& OutReason);
    static bool DuplicateSlot(UOriginPivotComponent* Pivot, FName Source, FName NewName, FText& OutReason);
    static bool RenameSlot(UOriginPivotComponent* Pivot, FName OldName, FName NewName, FText& OutReason);
    static bool ActivateSlot(UOriginPivotComponent* Pivot, FName Name, FText& OutReason);
    static bool SnapPivot(UOriginPivotComponent* Pivot, EOriginPivotSnapAction Action, FText& OutReason);

    /** Does not change anything. Candidate is relative for Blueprint templates, world for level instances. */
    static bool ComputeSnapCandidate(UOriginPivotComponent* Pivot, EOriginPivotSnapAction Action,
        FTransform& OutCandidate, FText& OutReason);
};
