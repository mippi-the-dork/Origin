#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "OriginSettings.generated.h"

UCLASS(Config = Origin, DefaultConfig, meta = (DisplayName = "Origin"))
class ORIGINEDITOR_API UOriginSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UOriginSettings();

    virtual FName GetSectionName() const override { return TEXT("Origin"); }
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    UPROPERTY(EditAnywhere, Config, Category = "Appearance|Hierarchy", meta = (DisplayName = "Parent Highlight Color", ToolTip = "World Outliner row highlight used when a drag will parent or move items into a destination actor or folder."))
    FLinearColor ParentHighlightColor;

    UPROPERTY(EditAnywhere, Config, Category = "Appearance|Hierarchy", meta = (DisplayName = "Deparent Highlight Color", ToolTip = "World Outliner row highlight used when a drag will detach items from their current actor parent."))
    FLinearColor DeparentHighlightColor;

    static const UOriginSettings* Get();
};
