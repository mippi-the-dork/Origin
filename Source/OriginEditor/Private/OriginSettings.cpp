#include "OriginSettings.h"

UOriginSettings::UOriginSettings()
    : ParentHighlightColor(FLinearColor(0.0f, 0.162029f, 0.745404f, 0.700000f))
    , DeparentHighlightColor(FLinearColor(0.745404f, 0.228181f, 0.0f, 0.700000f))
{
}

const UOriginSettings* UOriginSettings::Get()
{
    return GetDefault<UOriginSettings>();
}
