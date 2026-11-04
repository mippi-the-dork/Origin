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

    TestEqual(TEXT("Schema version"), Component->OriginPivotConfiguration.SchemaVersion, 2);
    TestEqual(TEXT("Exactly one Default slot"), Component->OriginPivotConfiguration.Slots.Num(), 1);
    TestEqual(TEXT("Active slot"), Component->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));

    FOriginPivotDefinition Definition;
    TestTrue(TEXT("Component Blueprint getter finds Default"),
        Component->GetOriginPivotSlotDefinition(TEXT("Default"), Definition));
    TestTrue(TEXT("Default position mode"), Definition.PositionMode == EOriginPivotMode::BoundsCenter);
    FTransform SlotPose;
    TestFalse(TEXT("Uncaptured Default does not claim a pose"),
        Component->GetOriginPivotSlotPose(TEXT("Default"), SlotPose));
    FText Reason;
    TestTrue(TEXT("Default can be captured explicitly"),
        Component->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    TestTrue(TEXT("Captured Default pose readable"),
        Component->GetOriginPivotSlotPose(TEXT("Default"), SlotPose));
    TestFalse(TEXT("Unknown slot lookup fails"),
        Component->GetOriginPivotSlotDefinition(TEXT("Missing"), Definition));
    TestEqual(TEXT("Lookup never creates slots"), Component->OriginPivotConfiguration.Slots.Num(), 1);
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginSlotNamesAndCopiesTest,
    "Origin.Runtime.Slots.NamesAndCopies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOriginSlotNamesAndCopiesTest::RunTest(const FString& Parameters)
{
    UOriginPivotComponent* Pivot = NewObject<UOriginPivotComponent>(GetTransientPackage());
    if (!TestNotNull(TEXT("Transient component"), Pivot)) return false;
    FText Reason;
    TestTrue(TEXT("Save initial Default"), Pivot->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    TestTrue(TEXT("Create Hinge"), Pivot->CreateOriginPivotSlot(TEXT("Hinge"), Reason));
    TestFalse(TEXT("Duplicate Hinge name rejected"), Pivot->CreateOriginPivotSlot(TEXT("Hinge"), Reason));
    TestTrue(TEXT("Duplicate preserves existing slot pose"), Pivot->DuplicateOriginPivotSlot(TEXT("Hinge"), TEXT("HingeCopy"), Reason));
    FTransform Source, Copy;
    TestTrue(TEXT("Source pose readable"), Pivot->GetOriginPivotSlotPose(TEXT("Hinge"), Source));
    TestTrue(TEXT("Copy pose readable"), Pivot->GetOriginPivotSlotPose(TEXT("HingeCopy"), Copy));
    TestTrue(TEXT("Copied transform matches"), Source.Equals(Copy, 1.e-5));
    TestTrue(TEXT("Rename active Hinge"), Pivot->RenameOriginPivotSlot(TEXT("Hinge"), TEXT("DoorHinge"), Reason));
    TestEqual(TEXT("Active name follows rename"),
        Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("DoorHinge")));
    TestFalse(TEXT("Default cannot be renamed"),
        Pivot->RenameOriginPivotSlot(TEXT("Default"), TEXT("Other"), Reason));
    TestTrue(TEXT("Delete inactive copy"), Pivot->DeleteOriginPivotSlot(TEXT("HingeCopy"), Reason));
    TArray<FName> Names;
    Pivot->GetOriginPivotSlotNames(Names);
    TestEqual(TEXT("Two slot names remain"), Names.Num(), 2);
    TestTrue(TEXT("Default still exists"), Pivot->OriginPivotConfiguration.FindSlot(TEXT("Default")) != nullptr);
    TestTrue(TEXT("Renamed slot still exists"), Pivot->OriginPivotConfiguration.FindSlot(TEXT("DoorHinge")) != nullptr);
    return true;
}
#endif
