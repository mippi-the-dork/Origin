// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginComponentDefaultSlotTest,
    "Origin.Runtime.ComponentDefaultSlotRead",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOriginComponentDefaultSlotTest::RunTest(const FString& Parameters)
{
    UOriginPivotComponent* Component = NewObject<UOriginPivotComponent>(GetTransientPackage());
    if (!TestNotNull(TEXT("Origin Pivot Component"), Component)) return false;

    TestEqual(TEXT("Schema version"), Component->OriginPivotConfiguration.SchemaVersion, 1);
    TestEqual(TEXT("Exactly one Default slot"), Component->OriginPivotConfiguration.Slots.Num(), 1);
    TestEqual(TEXT("Active slot"), Component->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));

    FOriginPivotDefinition Definition;
    TestTrue(TEXT("Component Blueprint getter finds Default"),
        Component->GetOriginPivotSlotDefinition(TEXT("Default"), Definition));
    TestTrue(TEXT("Default position mode"), Definition.PositionMode == EOriginPivotMode::BoundsCenter);
    TestFalse(TEXT("Unknown slot lookup fails"),
        Component->GetOriginPivotSlotDefinition(TEXT("Missing"), Definition));
    TestEqual(TEXT("Lookup never creates slots"), Component->OriginPivotConfiguration.Slots.Num(), 1);
    return true;
}
#endif
