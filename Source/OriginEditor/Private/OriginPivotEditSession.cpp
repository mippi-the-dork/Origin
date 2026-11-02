// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginPivotEditSession.h"

#include "OriginPivotComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "OriginPivotEditSession"

FOriginPivotEditSession::FOriginPivotEditSession(UOriginPivotComponent* InPivot, bool bInRecordUndo)
    : Pivot(InPivot), bRecordUndo(bInRecordUndo)
{
    if (IsValid(InPivot))
    {
        OriginalPivot = InPivot->GetComponentTransform();
        TArray<USceneComponent*> Children;
        InPivot->GetChildrenComponents(false, Children);
        for (USceneComponent* Child : Children)
        {
            if (IsValid(Child))
            {
                FSnapshot& Snapshot = OriginalDirectChildren.AddDefaulted_GetRef();
                Snapshot.Component = Child;
                Snapshot.World = Child->GetComponentTransform();
            }
        }
    }
}

FOriginPivotEditSession::~FOriginPivotEditSession()
{
    // A viewport mode may be destroyed on world switch or plugin shutdown.
    // Never leave a partially edited assembly or an open transaction behind.
    if (bBegun) Cancel();
}

void FOriginPivotEditSession::Begin()
{
    if (bBegun || !Pivot.IsValid()) return;
    bBegun = true;
    // Test fixtures disable global Undo recording to avoid altering the user's
    // editor transaction history, while still exercising Apply/Finish/Cancel.
    if (!bRecordUndo) return;
    Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("DragTransaction", "Origin: Edit Pivot (Preserve Children)"));
    UOriginPivotComponent* Target = Pivot.Get();
    TArray<USceneComponent*> Descendants;
    Target->GetChildrenComponents(true, Descendants);
    TSet<AActor*> Owners;
    if (AActor* Owner = Target->GetOwner()) Owners.Add(Owner);
    Target->Modify();
    for (USceneComponent* Child : Descendants)
    {
        if (IsValid(Child))
        {
            Child->Modify();
            if (AActor* Owner = Child->GetOwner()) Owners.Add(Owner);
        }
    }
    for (AActor* Owner : Owners) if (IsValid(Owner)) Owner->Modify();
}

EOriginPivotResult FOriginPivotEditSession::Apply(const FTransform& WorldTransform, FText& OutReason)
{
    UOriginPivotComponent* Target = Pivot.Get();
    if (!IsValid(Target))
    {
        OutReason = LOCTEXT("TargetGone", "The Origin Pivot is no longer valid.");
        return EOriginPivotResult::InvalidPivot;
    }
    // Preflight *before* opening an editor transaction or touching any UObject.
    const EOriginPivotResult Preflight = Target->CanSetOriginPivotWorldTransform(WorldTransform, OutReason);
    if (Preflight != EOriginPivotResult::Success)
    {
        if (Preflight != EOriginPivotResult::NoChange && bChanged) Cancel();
        return Preflight;
    }
    Begin();
    const EOriginPivotResult Result = Target->SetOriginPivotWorldTransform(WorldTransform, OutReason);
    if (Result == EOriginPivotResult::Success)
    {
        bChanged = true;
        Target->MarkPackageDirty();
        if (AActor* Owner = Target->GetOwner()) Owner->MarkPackageDirty();
    }
    else if (Result != EOriginPivotResult::NoChange)
    {
        // Recover the entire drag, not only the latest delta, on preservation failure.
        Cancel();
    }
    return Result;
}

void FOriginPivotEditSession::Finish()
{
    if (Transaction && !bChanged) Transaction->Cancel();
    Transaction.Reset();
    bBegun = false;
}

void FOriginPivotEditSession::Cancel()
{
    if (bChanged && Pivot.IsValid())
    {
        UOriginPivotComponent* Target = Pivot.Get();
        Target->SetWorldTransform(OriginalPivot, false, nullptr, ETeleportType::TeleportPhysics);
        for (const FSnapshot& Snapshot : OriginalDirectChildren)
        {
            if (USceneComponent* Child = Snapshot.Component.Get())
            {
                Child->SetWorldTransform(Snapshot.World, false, nullptr, ETeleportType::TeleportPhysics);
            }
        }
    }
    if (Transaction) Transaction->Cancel();
    Transaction.Reset();
    bChanged = false;
    bBegun = false;
}

#undef LOCTEXT_NAMESPACE
