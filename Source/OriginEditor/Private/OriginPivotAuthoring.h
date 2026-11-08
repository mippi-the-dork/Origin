// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#include "OriginPivotTypes.h"
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

    /** Source definition is stored in the Default slot without touching its captured pose. */
    static FOriginPivotDefinition ReadSourceDefinition(const UOriginPivotComponent* Pivot);
    static bool SaveSourceDefinition(UOriginPivotComponent* Pivot,
        const FOriginPivotDefinition& Definition, FText& OutReason);
    /** On-demand source evaluation uses the same pure math as Origin Anchors. */
    static bool ComputeSourceCandidate(UOriginPivotComponent* Pivot,
        const FOriginPivotDefinition& Definition, FTransform& OutCandidate, FText& OutReason);
    static bool RecalculateSourcePivot(UOriginPivotComponent* Pivot, FText& OutReason);
    static void ListDirectSourceComponents(UOriginPivotComponent* Pivot, TArray<FName>& OutNames);
    static void ListSourceSockets(UOriginPivotComponent* Pivot, FName ComponentName, TArray<FName>& OutNames);

    /** Does not change anything. Candidate is relative for Blueprint templates, world for level instances. */
    static bool ComputeSnapCandidate(UOriginPivotComponent* Pivot, EOriginPivotSnapAction Action,
        FTransform& OutCandidate, FText& OutReason);
};

// Alpha 5.4 source-rule controls, intentionally independent from named pose capture.
// Source definitions are saved on Default, but never overwrite captured slot poses.
