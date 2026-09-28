// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#pragma once
#include "IDetailCustomization.h"
class FOriginDetails final : public IDetailCustomization
{
public:
    virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
};
