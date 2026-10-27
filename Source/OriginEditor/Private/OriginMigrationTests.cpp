// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginAnchor.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginLegacySettingsMigrationTest,
    "Origin.Migration.LegacySettingsAndIdempotence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOriginLegacySettingsMigrationTest::RunTest(const FString& Parameters)
{
    // In-memory unit fixture. The persisted 1.0.6 map fixture is a separate
    // integration acceptance test and is intentionally not simulated here.
    AOriginAnchor* Anchor = NewObject<AOriginAnchor>(GetTransientPackage());
    if (!TestNotNull(TEXT("Transient Anchor"), Anchor)) return false;

    USceneComponent* RootBefore = Anchor->GetRootComponent();
    const FTransform TransformBefore = Anchor->GetActorTransform();
    Anchor->PivotMode = EOriginPivotMode::ComponentSocket;
    Anchor->CustomPivot = FVector(12.0, -23.0, 34.0);
    Anchor->BoundsAxes = FIntVector(0, 2, 1);
    Anchor->PivotComponentName = TEXT("DoorFrame");
    Anchor->PivotSocketName = TEXT("Hinge");
    Anchor->OriginPivotOffset = FVector(-9.0, 8.0, 7.0);
    Anchor->bLocalPivotOffset = true;
    const FSoftObjectPath ChildPath(TEXT("/Game/OriginTests/BP_Child.BP_Child"));
    const FSoftObjectPath ReferencePath(TEXT("/Game/OriginTests/BP_Reference.BP_Reference"));
    Anchor->PivotChild = TSoftObjectPtr<AActor>(ChildPath);
    Anchor->ReferenceActor = TSoftObjectPtr<AActor>(ReferencePath);

    Anchor->SyncLegacyPivotToRuntimeDefinition();
    TestEqual(TEXT("Current data version"), Anchor->OriginPivotConfiguration.SchemaVersion, 1);
    TestEqual(TEXT("Exactly one imported slot"), Anchor->OriginPivotConfiguration.Slots.Num(), 1);
    TestEqual(TEXT("Active slot"), Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));

    FOriginPivotDefinition Definition;
    if (!TestTrue(TEXT("Default slot accessible"),
        Anchor->GetOriginPivotSlotDefinition(TEXT("Default"), Definition))) return false;
    TestTrue(TEXT("Position mode"), Definition.PositionMode == EOriginPivotMode::ComponentSocket);
    TestTrue(TEXT("Custom position"), Definition.CustomPosition.Equals(Anchor->CustomPivot));
    TestTrue(TEXT("Bounds axes"), Definition.BoundsAxes == Anchor->BoundsAxes);
    TestTrue(TEXT("Component name"), Definition.ComponentName == Anchor->PivotComponentName);
    TestTrue(TEXT("Socket name"), Definition.SocketName == Anchor->PivotSocketName);
    TestTrue(TEXT("Offset"), Definition.Offset.Equals(Anchor->OriginPivotOffset));
    TestTrue(TEXT("Local offset"), Definition.bLocalOffset == Anchor->bLocalPivotOffset);
    TestTrue(TEXT("Child soft path preserved"), Definition.ChildActor.ToSoftObjectPath() == ChildPath);
    TestTrue(TEXT("Reference soft path preserved"), Definition.ReferenceActor.ToSoftObjectPath() == ReferencePath);

    Anchor->SyncLegacyPivotToRuntimeDefinition();
    TestEqual(TEXT("Idempotent: still one slot"), Anchor->OriginPivotConfiguration.Slots.Num(), 1);

    // Mimic the existing custom Details editing workflow.
    Anchor->PivotMode = EOriginPivotMode::BottomCenter;
    Anchor->OriginPivotOffset = FVector(0.0, 0.0, -45.0);
    Anchor->SyncLegacyPivotToRuntimeDefinition();
    FOriginPivotDefinition UpdatedDefinition;
    TestTrue(TEXT("Updated default slot found"), Anchor->GetOriginPivotSlotDefinition(TEXT("Default"), UpdatedDefinition));
    TestTrue(TEXT("Edit reflected in runtime slot"),
        UpdatedDefinition.PositionMode == EOriginPivotMode::BottomCenter &&
        UpdatedDefinition.Offset.Equals(Anchor->OriginPivotOffset));
    TestEqual(TEXT("No duplicate slots after edit"), Anchor->OriginPivotConfiguration.Slots.Num(), 1);
    TestTrue(TEXT("Root identity unchanged"), RootBefore == Anchor->GetRootComponent());
    TestTrue(TEXT("Anchor transform unchanged"), Anchor->GetActorTransform().Equals(TransformBefore));
    return true;
}
#endif
