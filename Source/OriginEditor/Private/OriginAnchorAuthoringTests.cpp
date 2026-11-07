// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Test the same Origin Anchor authoring entry point used by the Actor Details UI.
#include "OriginAnchorAuthoring.h"
#include "OriginPivotAuthoring.h"
#include "OriginPivotComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "OriginAnchor.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Editor/TransBuffer.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    struct FAnchorFixture
    {
        FTestWorldWrapper Wrapper;
        UWorld* World = nullptr;
        AOriginAnchor* Anchor = nullptr;
        AActor* Child = nullptr;

        bool Setup(FAutomationTestBase& Test)
        {
            if (!Test.TestTrue(TEXT("Isolated editor authoring world"),
                Wrapper.CreateTestWorld(EWorldType::Editor))) return false;
            World = Wrapper.GetTestWorld();
            if (!Test.TestNotNull(TEXT("Isolated world"), World)) return false;
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Anchor = World->SpawnActor<AOriginAnchor>(FVector(300, 75, 5), FRotator(0, 10, 0), Params);
            Child = World->SpawnActor<AActor>(FVector(450, 25, 15), FRotator(0, -20, 0), Params);
            if (!Test.TestNotNull(TEXT("Anchor"), Anchor) ||
                !Test.TestNotNull(TEXT("Child"), Child) ||
                !Test.TestNotNull(TEXT("Anchor root"), Anchor->GetRootComponent())) return false;
            Anchor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
            USceneComponent* ChildRoot = NewObject<USceneComponent>(Child, NAME_None, RF_Transactional);
            if (!Test.TestNotNull(TEXT("Child root"), ChildRoot)) return false;
            ChildRoot->SetMobility(EComponentMobility::Movable);
            Child->AddInstanceComponent(ChildRoot);
            Child->SetRootComponent(ChildRoot);
            ChildRoot->RegisterComponent();
            Child->SetActorLocation(FVector(450, 25, 15), false, nullptr, ETeleportType::TeleportPhysics);
            return Test.TestTrue(TEXT("Valid child attachment"),
                Child->AttachToActor(Anchor, FAttachmentTransformRules::KeepWorldTransform));
        }
    };

    bool Same(const FTransform& A, const FTransform& B)
    { return A.Equals(B, 0.01); }

    struct FScopedPrivateUndo
    {
        TStrongObjectPtr<UTransactor> Old;
        TStrongObjectPtr<UTransBuffer> Buffer;
        bool bInstalled = false;
        FScopedPrivateUndo()
        {
            if (!GEditor || !IsValid(GEditor->Trans.Get()) || GEditor->Trans->IsActive()) return;
            Old.Reset(GEditor->Trans.Get());
            UTransBuffer* Private = Cast<UTransBuffer>(GEditor->CreateTrans());
            if (!IsValid(Private)) { GEditor->Trans = Old.Get(); return; }
            Buffer.Reset(Private);
            GEditor->Trans = Private;
            bInstalled = true;
        }
        ~FScopedPrivateUndo()
        { if (bInstalled && GEditor) GEditor->Trans = Old.Get(); }
    };
}

#define ORIGIN_ANCHOR_AUTHORING_TEST(Class, Path) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Path, \
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_ANCHOR_AUTHORING_TEST(FOriginAnchorSlotCreateUndo,
    "Origin.Editor.Parity.AnchorSlotCreateUndoRedo")
bool FOriginAnchorSlotCreateUndo::RunTest(const FString&)
{
    FAnchorFixture F;
    if (!F.Setup(*this)) return false;
    FScopedPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Private transaction buffer"), Buffer)) return false;
    FText Reason;
    if (!TestTrue(TEXT("Same editor service as Details: create"),
        FOriginAnchorAuthoring::RunSlotAction(F.Anchor,EOriginAnchorSlotAction::Create,
            TEXT("Default"),TEXT("Hinge"),Reason)))
    { AddError(Reason.ToString()); return false; }
    if (!TestNotNull(TEXT("Created slot"), F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Hinge")))) return false;
    if (!TestTrue(TEXT("Undo slot creation"), Buffer->Undo(true))) return false;
    if (!TestNull(TEXT("Undo removes slot"), F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Hinge")))) return false;
    if (!TestTrue(TEXT("Redo slot creation"), Buffer->Redo())) return false;
    TestNotNull(TEXT("Redo restores slot"), F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Hinge")));
    TestEqual(TEXT("Redo restores active slot"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Hinge")));
    return true;
}

ORIGIN_ANCHOR_AUTHORING_TEST(FOriginAnchorActivateUndo,
    "Origin.Editor.Parity.AnchorSlotActivateUndoRedoPreservesChild")
bool FOriginAnchorActivateUndo::RunTest(const FString&)
{
    FAnchorFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    if (!TestTrue(TEXT("Prepare Default pose"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"),Reason))) return false;
    const FTransform First = F.Anchor->GetActorTransform();
    FTransform Second = First; Second.AddToTranslation(FVector(75,-35,10));
    if (!TestTrue(TEXT("Prepare second pose while child remains stationary"),
        F.Anchor->SetOriginPivotWorldTransform(Second,Reason) == EOriginPivotResult::Success))
    { AddError(Reason.ToString()); return false; }
    if (!TestTrue(TEXT("Capture alternate slot"), F.Anchor->CreateOriginPivotSlot(TEXT("Alternate"),Reason))) return false;
    const FTransform ChildBefore = F.Child->GetActorTransform();
    FScopedPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Private transaction buffer"), Buffer)) return false;
    for (int32 Cycle=0; Cycle<3; ++Cycle)
    {
        if (!TestTrue(TEXT("Editor activation"),
            FOriginAnchorAuthoring::RunSlotAction(F.Anchor,EOriginAnchorSlotAction::Activate,
                TEXT("Default"),NAME_None,Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestTrue(TEXT("Activated Default pose"), Same(F.Anchor->GetActorTransform(),First))) return false;
        if (!TestTrue(TEXT("Child remained stationary"), Same(F.Child->GetActorTransform(),ChildBefore))) return false;
        if (!TestTrue(TEXT("Undo activation"),Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo returns to alternate pose"), Same(F.Anchor->GetActorTransform(),Second))) return false;
        if (!TestTrue(TEXT("Undo restores active slot"),
            F.Anchor->OriginPivotConfiguration.ActiveSlotName == FName(TEXT("Alternate")))) return false;
        if (!TestTrue(TEXT("Redo activation"),Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo returns to Default pose"), Same(F.Anchor->GetActorTransform(),First))) return false;
        if (!TestTrue(TEXT("Redo leaves child stationary"), Same(F.Child->GetActorTransform(),ChildBefore))) return false;
        if (!TestTrue(TEXT("Undo for next activation"),Buffer->Undo(true))) return false;
    }
    return true;
}

ORIGIN_ANCHOR_AUTHORING_TEST(FOriginAnchorStaticSlotActivation,
    "Origin.Editor.Parity.StaticAnchorSlotActivateUndoRedo")
bool FOriginAnchorStaticSlotActivation::RunTest(const FString&)
{
    FAnchorFixture F;
    if (!F.Setup(*this)) return false;
    F.Anchor->GetRootComponent()->SetMobility(EComponentMobility::Static);
    FText Reason;
    if (!TestTrue(TEXT("Capture Default on a Static Anchor"),
        F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason))) return false;
    const FTransform InitialAnchor = F.Anchor->GetActorTransform();
    const FTransform InitialChild = F.Child->GetActorTransform();
    const FVector NextLocation = InitialAnchor.GetLocation() + FVector(50, 30, 8);
    // Stage a second valid pose without leaving the attached child displaced.
    F.Anchor->SetActorLocation(NextLocation, false, nullptr, ETeleportType::TeleportPhysics);
    F.Child->SetActorTransform(InitialChild, false, nullptr, ETeleportType::TeleportPhysics);
    if (!TestTrue(TEXT("Capture alternate pose on Static Anchor"),
        F.Anchor->CreateOriginPivotSlot(TEXT("Alternate"), Reason))) return false;
    const FTransform AlternateAnchor = F.Anchor->GetActorTransform();
    FScopedPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Private transaction buffer"), Buffer)) return false;
    for (int32 Cycle=0; Cycle<2; ++Cycle)
    {
        if (!TestTrue(TEXT("Editor-only Static Anchor slot activation"),
            FOriginAnchorAuthoring::RunSlotAction(F.Anchor, EOriginAnchorSlotAction::Activate,
                TEXT("Default"),NAME_None, Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestTrue(TEXT("Static Anchor returns to Default"),
            Same(F.Anchor->GetActorTransform(), InitialAnchor))) return false;
        if (!TestTrue(TEXT("Movable child remains stationary"),
            Same(F.Child->GetActorTransform(), InitialChild))) return false;
        if (!TestTrue(TEXT("Undo Static Anchor activation"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo returns to alternate pose"),
            Same(F.Anchor->GetActorTransform(), AlternateAnchor))) return false;
        if (!TestTrue(TEXT("Redo Static Anchor activation"), Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo preserves child"),
            Same(F.Child->GetActorTransform(), InitialChild))) return false;
        if (!TestTrue(TEXT("Undo before next cycle"), Buffer->Undo(true))) return false;
    }
    return true;
}

ORIGIN_ANCHOR_AUTHORING_TEST(FOriginAnchorInvalidSlot,
    "Origin.Editor.Parity.RejectedAnchorSlotActivationIsAtomic")
bool FOriginAnchorInvalidSlot::RunTest(const FString&)
{
    FAnchorFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    const FTransform Before = F.Anchor->GetActorTransform();
    const FName OldActive = F.Anchor->OriginPivotConfiguration.ActiveSlotName;
    TestFalse(TEXT("Editor refuses uncaptured Default"),
        FOriginAnchorAuthoring::RunSlotAction(F.Anchor,EOriginAnchorSlotAction::Activate,
            TEXT("Default"),NAME_None,Reason));
    TestTrue(TEXT("Rejection reason available"),!Reason.IsEmpty());
    TestTrue(TEXT("Anchor transform unchanged"),Same(F.Anchor->GetActorTransform(),Before));
    TestEqual(TEXT("Active slot unchanged"),F.Anchor->OriginPivotConfiguration.ActiveSlotName,OldActive);
    TestTrue(TEXT("Capture valid Default"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    FOriginPivotSlot* Slot = F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Default"));
    if (!TestNotNull(TEXT("Captured slot"), Slot)) return false;
    Slot->CapturedSocketName = TEXT("MissingSocket");
    TestFalse(TEXT("Editor service rejects changed socket frame"),
        FOriginAnchorAuthoring::RunSlotAction(F.Anchor,EOriginAnchorSlotAction::Activate,
            TEXT("Default"),NAME_None,Reason));
    TestTrue(TEXT("Socket rejection is atomic"),Same(F.Anchor->GetActorTransform(),Before));
    return true;
}

ORIGIN_ANCHOR_AUTHORING_TEST(FOriginAnchorDeleteActiveUndo,
    "Origin.Editor.Parity.AnchorSlotDeleteActiveUndoRedo")
bool FOriginAnchorDeleteActiveUndo::RunTest(const FString&)
{
    FAnchorFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    if (!TestTrue(TEXT("Create active named Anchor slot"),
        FOriginAnchorAuthoring::RunSlotAction(F.Anchor, EOriginAnchorSlotAction::Create,
            TEXT("Default"), TEXT("Temporary"), Reason)))
    { AddError(Reason.ToString()); return false; }
    const FTransform AnchorBefore = F.Anchor->GetActorTransform();
    const FTransform ChildBefore = F.Child->GetActorTransform();
    FScopedPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Private transaction buffer"), Buffer)) return false;
    if (!TestTrue(TEXT("Delete active via actual Anchor Details service"),
        FOriginAnchorAuthoring::RunSlotAction(F.Anchor, EOriginAnchorSlotAction::Delete,
            TEXT("Temporary"), NAME_None, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Only Default remains"), F.Anchor->OriginPivotConfiguration.Slots.Num(), 1);
    TestEqual(TEXT("Active selection becomes Default"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestTrue(TEXT("Anchor did not move"), Same(F.Anchor->GetActorTransform(), AnchorBefore));
    TestTrue(TEXT("Attached Actor did not move"), Same(F.Child->GetActorTransform(), ChildBefore));
    TestEqual(TEXT("Single Undo transaction"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo active deletion"), Buffer->Undo(true))) return false;
    TestNotNull(TEXT("Undo restores named slot"), F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Temporary")));
    TestEqual(TEXT("Undo restores active name"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Temporary")));
    if (!TestTrue(TEXT("Redo active deletion"), Buffer->Redo())) return false;
    TestNull(TEXT("Redo deletes named slot"), F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Temporary")));
    TestEqual(TEXT("Redo selects Default"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestTrue(TEXT("Undo/Redo leaves Anchor in place"), Same(F.Anchor->GetActorTransform(), AnchorBefore));
    TestTrue(TEXT("Undo/Redo leaves attached Actor in place"), Same(F.Child->GetActorTransform(), ChildBefore));
    return true;
}

ORIGIN_ANCHOR_AUTHORING_TEST(FOriginComponentDeleteActiveUndo,
    "Origin.Editor.Parity.ComponentSlotDeleteActiveUndoRedo")
bool FOriginComponentDeleteActiveUndo::RunTest(const FString&)
{
    FAnchorFixture F;
    if (!F.Setup(*this)) return false;
    UOriginPivotComponent* Pivot = NewObject<UOriginPivotComponent>(F.Child, NAME_None, RF_Transactional);
    if (!TestNotNull(TEXT("Component Pivot"), Pivot)) return false;
    F.Child->AddInstanceComponent(Pivot);
    Pivot->SetupAttachment(F.Child->GetRootComponent());
    Pivot->RegisterComponent();
    const FTransform PivotBefore = Pivot->GetComponentTransform();
    const FTransform ActorBefore = F.Child->GetActorTransform();
    FText Reason;
    if (!TestTrue(TEXT("Create active component slot through authoring service"),
        FOriginPivotAuthoring::CreateSlot(Pivot, TEXT("Temporary"), Reason)))
    { AddError(Reason.ToString()); return false; }
    FScopedPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Private transaction buffer"), Buffer)) return false;
    if (!TestTrue(TEXT("Delete active via actual component Details service"),
        FOriginPivotAuthoring::RemoveSlot(Pivot, TEXT("Temporary"), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Component only has Default"), Pivot->OriginPivotConfiguration.Slots.Num(), 1);
    TestEqual(TEXT("Component active resets to Default"), Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestTrue(TEXT("Component did not move"), Same(Pivot->GetComponentTransform(), PivotBefore));
    TestTrue(TEXT("Owning Actor did not move"), Same(F.Child->GetActorTransform(), ActorBefore));
    TestEqual(TEXT("Single Undo transaction"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo component slot deletion"), Buffer->Undo(true))) return false;
    TestNotNull(TEXT("Undo restores named component slot"), Pivot->OriginPivotConfiguration.FindSlot(TEXT("Temporary")));
    TestEqual(TEXT("Undo restores component active slot"), Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Temporary")));
    if (!TestTrue(TEXT("Redo component slot deletion"), Buffer->Redo())) return false;
    TestNull(TEXT("Redo deletes named component slot"), Pivot->OriginPivotConfiguration.FindSlot(TEXT("Temporary")));
    TestEqual(TEXT("Redo resets component active slot"), Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestTrue(TEXT("Undo/Redo leaves component in place"), Same(Pivot->GetComponentTransform(), PivotBefore));
    return true;
}

#undef ORIGIN_ANCHOR_AUTHORING_TEST
#endif
