// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"

/** Focused Origin Pivot Component controls. The serialized slot array is not a user-facing editor. */
class FOriginPivotComponentDetails final : public IDetailCustomization
{
public:
    virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
};
