// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
class AOriginAnchor;

enum class EOriginAnchorSlotAction : uint8
{
    Create, Capture, Activate, Duplicate, Rename, Delete
};

/** The same transactional editor adapter is used by Details controls and native automation. */
struct FOriginAnchorAuthoring
{
    static bool CanAuthor(AOriginAnchor* Anchor, FText& Reason);
    static bool RunSlotAction(AOriginAnchor* Anchor, EOriginAnchorSlotAction Action,
        FName SelectedName, FName NewName, FText& Reason);
};
