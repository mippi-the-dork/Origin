// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class SSceneOutliner;

namespace OriginOutlinerToolbar
{
    // SSceneOutliner does not expose an outliner-toolbar extender in UE 5.8.
    // Add Origin's single button to the existing toolbar after construction,
    // leaving the native actor browsing mode and World Outliner factory intact.
    void Schedule(const TSharedRef<SSceneOutliner>& Outliner);
}
