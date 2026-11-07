// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponent.h"
#include "OriginPivotSlots.h"

UOriginPivotComponent::UOriginPivotComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetMobility(EComponentMobility::Movable);
    OriginPivotConfiguration.SchemaVersion = 2;
    OriginPivotConfiguration.Slots.AddDefaulted();
}

bool UOriginPivotComponent::GetOriginPivotSlotDefinition(FName Name, FOriginPivotDefinition& Out) const
{
    return FOriginPivotSlots::Definition(OriginPivotConfiguration, Name, Out);
}

bool UOriginPivotComponent::CreateOriginPivotSlot(FName Name, FText& Reason)
{
    return FOriginPivotSlots::Create(OriginPivotConfiguration, Name, GetRelativeTransform(), false, GetAttachSocketName(), Reason);
}

bool UOriginPivotComponent::CaptureOriginPivotSlot(FName Name, FText& Reason)
{
    return FOriginPivotSlots::Capture(OriginPivotConfiguration, Name, GetRelativeTransform(), false, GetAttachSocketName(), Reason);
}

bool UOriginPivotComponent::DuplicateOriginPivotSlot(FName Source, FName Name, FText& Reason)
{
    return FOriginPivotSlots::Duplicate(OriginPivotConfiguration, Source, Name, Reason);
}

bool UOriginPivotComponent::RenameOriginPivotSlot(FName OldName, FName NewName, FText& Reason)
{
    return FOriginPivotSlots::Rename(OriginPivotConfiguration, OldName, NewName, Reason);
}

void UOriginPivotComponent::GetOriginPivotSlotNames(TArray<FName>& Out) const
{
    FOriginPivotSlots::Names(OriginPivotConfiguration, Out);
}

bool UOriginPivotComponent::DeleteOriginPivotSlot(FName Name, FText& Reason)
{
    return FOriginPivotSlots::Delete(OriginPivotConfiguration, Name, Reason);
}

bool UOriginPivotComponent::GetOriginPivotSlotPose(FName Name, FTransform& Out) const
{
    return FOriginPivotSlots::Pose(OriginPivotConfiguration, Name, Out);
}

EOriginPivotResult UOriginPivotComponent::ActivateOriginPivotSlot(FName SlotName, FText& FailureReason)
{
    FTransform RelativePose;
    if (!GetOriginPivotSlotPose(SlotName, RelativePose))
    {
        FailureReason = FText::FromString(TEXT("This pivot slot is missing or has never captured a transform."));
        return EOriginPivotResult::InvalidTransform;
    }
    if (IsTemplate())
    {
        FailureReason = FText::FromString(TEXT("Blueprint component templates must be activated through Origin's Blueprint Editor authoring controls."));
        return EOriginPivotResult::InvalidPivot;
    }
    if (!GetAttachParent())
    {
        FailureReason = FText::FromString(TEXT("Runtime root pivots have no stable parent-local frame. Attach Origin Pivot below a Scene Component root before activating stored slots."));
        return EOriginPivotResult::InvalidPivot;
    }
    FTransform Candidate = RelativePose;
    if (const USceneComponent* Parent = GetAttachParent())
    {
        const FName SocketName = GetAttachSocketName();
        const FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(SlotName);
        if (!Slot || Slot->CapturedSocketName != SocketName ||
            (!SocketName.IsNone() && !Parent->DoesSocketExist(SocketName)))
        {
            FailureReason = FText::FromString(TEXT("The pivot attachment socket changed or is missing. Recapture the slot in the current attachment frame."));
            return EOriginPivotResult::InvalidPivot;
        }
        Candidate = RelativePose * Parent->GetSocketTransform(SocketName, RTS_World);
    }

    // Preflight before moving anything or changing the active slot name.
    const EOriginPivotResult Preflight = CanSetOriginPivotWorldTransform(Candidate, FailureReason);
    if (Preflight != EOriginPivotResult::Success && Preflight != EOriginPivotResult::NoChange)
        return Preflight;
    const EOriginPivotResult Result = Preflight == EOriginPivotResult::NoChange
        ? Preflight : SetOriginPivotWorldTransform(Candidate, FailureReason);
    if (Result == EOriginPivotResult::Success || Result == EOriginPivotResult::NoChange)
    {
        OriginPivotConfiguration.ActiveSlotName = SlotName;
        FailureReason = FText::GetEmpty();
    }
    return Result;
}

EOriginPivotResult UOriginPivotComponent::CanSetOriginPivotWorldTransform(
    const FTransform& NewPivotWorldTransform, FText& FailureReason) const
{
    return FOriginPivotCore::ValidatePivotEdit(this, NewPivotWorldTransform, FailureReason);
}

EOriginPivotResult UOriginPivotComponent::SetOriginPivotWorldTransform(
    const FTransform& NewPivotWorldTransform, FText& FailureReason)
{
    const FTransform Before = GetComponentTransform();
    const EOriginPivotResult Result = FOriginPivotCore::SetPivotWorldTransformPreservingChildren(
        this, NewPivotWorldTransform, FailureReason);
    if (Result == EOriginPivotResult::Success)
    {
        OnOriginPivotChanged.Broadcast(Before, GetComponentTransform());
    }
    return Result;
}
