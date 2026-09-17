#pragma once
#include "IDetailCustomization.h"
class FOriginDetails final : public IDetailCustomization
{
public:
    virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
};
