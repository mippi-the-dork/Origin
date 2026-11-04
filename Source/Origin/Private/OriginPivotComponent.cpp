// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponent.h"

UOriginPivotComponent::UOriginPivotComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetMobility(EComponentMobility::Movable);
    OriginPivotConfiguration.SchemaVersion = 2;
    OriginPivotConfiguration.Slots.AddDefaulted();
}

bool UOriginPivotComponent::GetOriginPivotSlotDefinition(FName SlotName, FOriginPivotDefinition& OutDefinition) const
{
    if (const FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(SlotName))
    {
        OutDefinition = Slot->Definition;
        return true;
    }
    return false;
}

namespace
{
    bool ValidSlotName(FName Name)
    {
        return !Name.IsNone() && !Name.ToString().TrimStartAndEnd().IsEmpty();
    }

    bool ValidSlotPose(const FTransform& Pose)
    {
        return Pose.IsValid() && !Pose.ContainsNaN() && Pose.GetRotation().IsNormalized();
    }
}

bool UOriginPivotComponent::CreateOriginPivotSlot(FName SlotName, FText& FailureReason)
{
    if (!ValidSlotName(SlotName) || OriginPivotConfiguration.FindSlot(SlotName))
    {
        FailureReason = FText::FromString(TEXT("Enter a unique, nonempty pivot slot name."));
        return false;
    }
    const FTransform Pose = GetRelativeTransform();
    if (!ValidSlotPose(Pose))
    {
        FailureReason = FText::FromString(TEXT("The pivot has an invalid transform; no slot was saved."));
        return false;
    }
    FOriginPivotSlot& Added = OriginPivotConfiguration.Slots.AddDefaulted_GetRef();
    Added.Name = SlotName;
    Added.bHasSavedRelativeTransform = true;
    Added.SavedRelativeTransform = Pose;
    OriginPivotConfiguration.SchemaVersion = 2;
    OriginPivotConfiguration.ActiveSlotName = SlotName;
    FailureReason = FText::GetEmpty();
    return true;
}

bool UOriginPivotComponent::CaptureOriginPivotSlot(FName SlotName, FText& FailureReason)
{
    FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(SlotName);
    if (!Slot)
    {
        FailureReason = FText::FromString(TEXT("The specified pivot slot does not exist."));
        return false;
    }
    const FTransform Pose = GetRelativeTransform();
    if (!ValidSlotPose(Pose))
    {
        FailureReason = FText::FromString(TEXT("The pivot has an invalid transform; no slot was changed."));
        return false;
    }
    Slot->SavedRelativeTransform = Pose;
    Slot->bHasSavedRelativeTransform = true;
    OriginPivotConfiguration.SchemaVersion = 2;
    OriginPivotConfiguration.ActiveSlotName = SlotName;
    FailureReason = FText::GetEmpty();
    return true;
}

bool UOriginPivotComponent::DuplicateOriginPivotSlot(FName SourceName, FName NewName, FText& FailureReason)
{
    // Copy before adding: the array can reallocate when a new slot is added.
    const FOriginPivotSlot* Source = OriginPivotConfiguration.FindSlot(SourceName);
    if (!Source || !ValidSlotName(NewName) || OriginPivotConfiguration.FindSlot(NewName))
    {
        FailureReason = FText::FromString(TEXT("Choose an existing source and a unique, nonempty new slot name."));
        return false;
    }
    FOriginPivotSlot Copy = *Source;
    Copy.Name = NewName;
    OriginPivotConfiguration.Slots.Add(MoveTemp(Copy));
    OriginPivotConfiguration.SchemaVersion = 2;
    FailureReason = FText::GetEmpty();
    return true;
}

bool UOriginPivotComponent::RenameOriginPivotSlot(FName OldName, FName NewName, FText& FailureReason)
{
    FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(OldName);
    if (!Slot || OldName == FName(TEXT("Default")) || !ValidSlotName(NewName) ||
        OriginPivotConfiguration.FindSlot(NewName))
    {
        FailureReason = FText::FromString(TEXT("Default cannot be renamed. Enter a unique, nonempty name for an existing slot."));
        return false;
    }
    Slot->Name = NewName;
    if (OriginPivotConfiguration.ActiveSlotName == OldName)
        OriginPivotConfiguration.ActiveSlotName = NewName;
    FailureReason = FText::GetEmpty();
    return true;
}

void UOriginPivotComponent::GetOriginPivotSlotNames(TArray<FName>& OutNames) const
{
    OutNames.Reset();
    for (const FOriginPivotSlot& Slot : OriginPivotConfiguration.Slots)
        OutNames.Add(Slot.Name);
}

bool UOriginPivotComponent::DeleteOriginPivotSlot(FName SlotName, FText& FailureReason)
{
    if (SlotName.IsNone() || SlotName == FName(TEXT("Default")) ||
        SlotName == OriginPivotConfiguration.ActiveSlotName)
    {
        FailureReason = FText::FromString(TEXT("The Default slot and the active slot cannot be deleted. Activate a different slot first."));
        return false;
    }
    const int32 Index = OriginPivotConfiguration.Slots.IndexOfByPredicate(
        [SlotName](const FOriginPivotSlot& Slot) { return Slot.Name == SlotName; });
    if (Index == INDEX_NONE)
    {
        FailureReason = FText::FromString(TEXT("The specified pivot slot does not exist."));
        return false;
    }
    OriginPivotConfiguration.Slots.RemoveAt(Index);
    FailureReason = FText::GetEmpty();
    return true;
}

bool UOriginPivotComponent::GetOriginPivotSlotPose(FName SlotName, FTransform& OutRelativePose) const
{
    const FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(SlotName);
    if (!Slot || !Slot->bHasSavedRelativeTransform || !ValidSlotPose(Slot->SavedRelativeTransform))
        return false;
    OutRelativePose = Slot->SavedRelativeTransform;
    return true;
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
        Candidate = RelativePose * Parent->GetComponentTransform();

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
