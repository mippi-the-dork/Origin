// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class IInputProcessor;
class ISceneOutliner;

namespace OriginHierarchyFeedback
{
    void RegisterOutliner(ISceneOutliner& Outliner);
    TSharedRef<IInputProcessor> CreateInputProcessor();
    void Shutdown();
}
