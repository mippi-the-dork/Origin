// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "OriginPivotCore.h"

class UOriginPivotComponent;
class USceneComponent;
class FScopedTransaction;

/**
 * One editor pivot edit/drag, with one undo entry and rollback on cancellation.
 * Uses the same runtime preservation core as Blueprint; never modifies actor identity.
 * The caller owns the lifetime and explicitly calls Finish or Cancel.
 */
class FOriginPivotEditSession final
{
public:
    explicit FOriginPivotEditSession(UOriginPivotComponent* InPivot, bool bInRecordUndo = true);
    ~FOriginPivotEditSession();

    EOriginPivotResult Apply(const FTransform& WorldTransform, FText& OutReason);
    void Finish();
    void Cancel();
    bool HasChanged() const { return bChanged; }

private:
    struct FSnapshot
    {
        TWeakObjectPtr<USceneComponent> Component;
        FTransform World;
    };

    void Begin();
    TWeakObjectPtr<UOriginPivotComponent> Pivot;
    FTransform OriginalPivot;
    TArray<FSnapshot> OriginalDirectChildren;
    TUniquePtr<FScopedTransaction> Transaction;
    bool bChanged = false;
    bool bRecordUndo = true;
    bool bBegun = false;
};
