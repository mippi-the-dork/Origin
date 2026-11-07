// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginPivotSlots.h"

namespace
{
    bool ValidName(FName Name)
    {
        return !Name.IsNone() && !Name.ToString().TrimStartAndEnd().IsEmpty();
    }

    bool ValidPose(const FTransform& Pose)
    {
        return Pose.IsValid() && !Pose.ContainsNaN() && Pose.GetRotation().IsNormalized();
    }
}

bool FOriginPivotSlots::Create(FOriginPivotConfiguration& Config, FName Name,
    const FTransform& InPose, bool bWorldFrame, FName SocketName, FText& Reason)
{
    if (!ValidName(Name) || Config.FindSlot(Name))
    {
        Reason = FText::FromString(TEXT("Enter a unique, nonempty pivot slot name."));
        return false;
    }
    if (!ValidPose(InPose))
    {
        Reason = FText::FromString(TEXT("The pivot has an invalid transform; no slot was saved."));
        return false;
    }
    FOriginPivotSlot& Added = Config.Slots.AddDefaulted_GetRef();
    Added.Name = Name;
    Added.bHasSavedRelativeTransform = true;
    Added.SavedRelativeTransform = InPose;
    Added.bCapturedAtWorldRoot = bWorldFrame;
    Added.CapturedSocketName = SocketName;
    Config.SchemaVersion = 2;
    Config.ActiveSlotName = Name;
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginPivotSlots::Capture(FOriginPivotConfiguration& Config, FName Name,
    const FTransform& InPose, bool bWorldFrame, FName SocketName, FText& Reason)
{
    FOriginPivotSlot* Slot = Config.FindSlot(Name);
    if (!Slot)
    {
        Reason = FText::FromString(TEXT("The specified pivot slot does not exist."));
        return false;
    }
    if (!ValidPose(InPose))
    {
        Reason = FText::FromString(TEXT("The pivot has an invalid transform; no slot was changed."));
        return false;
    }
    Slot->SavedRelativeTransform = InPose;
    Slot->bHasSavedRelativeTransform = true;
    Slot->bCapturedAtWorldRoot = bWorldFrame;
    Slot->CapturedSocketName = SocketName;
    Config.SchemaVersion = 2;
    Config.ActiveSlotName = Name;
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginPivotSlots::Duplicate(FOriginPivotConfiguration& Config, FName SourceName,
    FName NewName, FText& Reason)
{
    const FOriginPivotSlot* Source = Config.FindSlot(SourceName);
    if (!Source || !ValidName(NewName) || Config.FindSlot(NewName))
    {
        Reason = FText::FromString(TEXT("Choose an existing source and a unique, nonempty new slot name."));
        return false;
    }
    FOriginPivotSlot Copy = *Source; // Array addition may invalidate the original pointer.
    Copy.Name = NewName;
    Config.Slots.Add(MoveTemp(Copy));
    Config.SchemaVersion = 2;
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginPivotSlots::Rename(FOriginPivotConfiguration& Config, FName OldName,
    FName NewName, FText& Reason)
{
    FOriginPivotSlot* Slot = Config.FindSlot(OldName);
    if (!Slot || OldName == FName(TEXT("Default")) || !ValidName(NewName) ||
        Config.FindSlot(NewName))
    {
        Reason = FText::FromString(TEXT("Default cannot be renamed. Enter a unique, nonempty name for an existing slot."));
        return false;
    }
    Slot->Name = NewName;
    if (Config.ActiveSlotName == OldName) Config.ActiveSlotName = NewName;
    Config.SchemaVersion = 2;
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginPivotSlots::Delete(FOriginPivotConfiguration& Config, FName Name, FText& Reason)
{
    if (Name.IsNone() || Name == FName(TEXT("Default")))
    {
        Reason = FText::FromString(TEXT("The required Default slot cannot be deleted."));
        return false;
    }
    const int32 Index = Config.Slots.IndexOfByPredicate(
        [Name](const FOriginPivotSlot& Slot) { return Slot.Name == Name; });
    if (Index == INDEX_NONE)
    {
        Reason = FText::FromString(TEXT("The specified pivot slot does not exist."));
        return false;
    }
    const bool bDeletingActive = Config.ActiveSlotName == Name;
    if (bDeletingActive && !Config.FindSlot(TEXT("Default")))
    {
        Reason = FText::FromString(TEXT("Cannot delete the active slot: the required Default slot is missing."));
        return false;
    }
    // Active identifies the most recently chosen pose; it is not an attachment
    // dependency. Returning to Default changes only the selection, not the pivot.
    Config.Slots.RemoveAt(Index);
    if (bDeletingActive) Config.ActiveSlotName = TEXT("Default");
    Config.SchemaVersion = 2;
    Reason = FText::GetEmpty();
    return true;
}

void FOriginPivotSlots::Names(const FOriginPivotConfiguration& Config, TArray<FName>& OutNames)
{
    OutNames.Reset();
    for (const FOriginPivotSlot& Slot : Config.Slots) OutNames.Add(Slot.Name);
}

bool FOriginPivotSlots::Pose(const FOriginPivotConfiguration& Config, FName Name,
    FTransform& OutPose, bool* bOutWorldFrame)
{
    const FOriginPivotSlot* Slot = Config.FindSlot(Name);
    if (!Slot || !Slot->bHasSavedRelativeTransform || !ValidPose(Slot->SavedRelativeTransform))
        return false;
    OutPose = Slot->SavedRelativeTransform;
    if (bOutWorldFrame) *bOutWorldFrame = Slot->bCapturedAtWorldRoot;
    return true;
}

bool FOriginPivotSlots::Definition(const FOriginPivotConfiguration& Config, FName Name,
    FOriginPivotDefinition& OutDefinition)
{
    if (const FOriginPivotSlot* Slot = Config.FindSlot(Name))
    {
        OutDefinition = Slot->Definition;
        return true;
    }
    return false;
}
