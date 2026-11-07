// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "OriginPivotTypes.h"

/**
 * Named-slot data rules shared by Origin Anchor Actors and Origin Pivot Components.
 * Purely edits serialized slot data: no attachment/transform movement, Tick, or editor dependencies.
 * The caller supplies the pose in its actual attachment-parent frame, plus whether that
 * frame is the world (an unattached Anchor). No implicit pose is invented for legacy slots.
 */
struct ORIGIN_API FOriginPivotSlots
{
    static bool Create(FOriginPivotConfiguration& Config, FName Name,
        const FTransform& Pose, bool bWorldFrame, FName SocketName, FText& Reason);
    static bool Capture(FOriginPivotConfiguration& Config, FName Name,
        const FTransform& Pose, bool bWorldFrame, FName SocketName, FText& Reason);
    static bool Duplicate(FOriginPivotConfiguration& Config, FName Source, FName NewName, FText& Reason);
    static bool Rename(FOriginPivotConfiguration& Config, FName OldName, FName NewName, FText& Reason);
    static bool Delete(FOriginPivotConfiguration& Config, FName Name, FText& Reason);
    static void Names(const FOriginPivotConfiguration& Config, TArray<FName>& OutNames);
    static bool Pose(const FOriginPivotConfiguration& Config, FName Name,
        FTransform& OutPose, bool* bOutWorldFrame = nullptr);
    static bool Definition(const FOriginPivotConfiguration& Config, FName Name,
        FOriginPivotDefinition& OutDefinition);
};
