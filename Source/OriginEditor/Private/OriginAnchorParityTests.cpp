// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Origin 2.0 Alpha 5.3: both pivot types must obey the same slot-data and
// runtime preservation contract. Uses native Unreal test worlds, never user maps.
#include "OriginAnchor.h"
#include "OriginPivotComponent.h"
#include "OriginPivotSlots.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    bool Same(const FTransform& A, const FTransform& B)
    {
        return A.GetLocation().Equals(B.GetLocation(), 0.01)
            && A.GetRotation().Equals(B.GetRotation(), 1.e-4)
            && A.GetScale3D().Equals(B.GetScale3D(), 1.e-4);
    }

    bool Accepted(EOriginPivotResult Result)
    {
        return Result == EOriginPivotResult::Success || Result == EOriginPivotResult::NoChange;
    }

    struct FParityFixture
    {
        FTestWorldWrapper Wrapper;
        UWorld* World = nullptr;
        AOriginAnchor* Anchor = nullptr;
        AActor* AnchorChild = nullptr;
        AActor* AnchorGrandchild = nullptr;
        AActor* PivotOwner = nullptr;
        UOriginPivotComponent* Pivot = nullptr;
        AActor* PivotChild = nullptr;

        AActor* SpawnSceneActor(const FVector& Location)
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* Actor = World->SpawnActor<AActor>(Location, FRotator(0, 15, 0), Params);
            if (!Actor) return nullptr;
            USceneComponent* Root = NewObject<USceneComponent>(Actor, NAME_None, RF_Transactional);
            if (!Root) return nullptr;
            Root->SetMobility(EComponentMobility::Movable);
            Actor->AddInstanceComponent(Root);
            Actor->SetRootComponent(Root);
            Root->RegisterComponent();
            Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
            return Actor;
        }

        bool Setup(FAutomationTestBase& Test)
        {
            if (!Test.TestTrue(TEXT("Isolated Origin parity test world"),
                Wrapper.CreateTestWorld(EWorldType::Game))) return false;
            World = Wrapper.GetTestWorld();
            if (!Test.TestNotNull(TEXT("Isolated test world"), World)) return false;
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Anchor = World->SpawnActor<AOriginAnchor>(FVector(310, 20, 5), FRotator(0, 25, 0), Params);
            if (!Test.TestNotNull(TEXT("Anchor"), Anchor) ||
                !Test.TestNotNull(TEXT("Anchor root"), Anchor->GetRootComponent())) return false;
            Anchor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
            AnchorChild = SpawnSceneActor(FVector(380, 50, 10));
            AnchorGrandchild = SpawnSceneActor(FVector(420, 80, 25));
            PivotOwner = SpawnSceneActor(FVector(-210, -45, 5));
            PivotChild = SpawnSceneActor(FVector(-100, 30, 30));
            if (!Test.TestNotNull(TEXT("Anchor child"), AnchorChild) ||
                !Test.TestNotNull(TEXT("Anchor grandchild"), AnchorGrandchild) ||
                !Test.TestNotNull(TEXT("Pivot owner"), PivotOwner) ||
                !Test.TestNotNull(TEXT("Pivot child"), PivotChild)) return false;
            Pivot = NewObject<UOriginPivotComponent>(PivotOwner, NAME_None, RF_Transactional);
            if (!Test.TestNotNull(TEXT("Origin Pivot component"), Pivot)) return false;
            PivotOwner->AddInstanceComponent(Pivot);
            Pivot->SetupAttachment(PivotOwner->GetRootComponent());
            Pivot->RegisterComponent();
            Pivot->SetRelativeLocation(FVector(30, 15, 0));
            return Test.TestTrue(TEXT("Anchor child attaches"),
                    AnchorChild->AttachToActor(Anchor, FAttachmentTransformRules::KeepWorldTransform)) &&
                Test.TestTrue(TEXT("Nested Actor attaches"),
                    AnchorGrandchild->AttachToActor(AnchorChild, FAttachmentTransformRules::KeepWorldTransform)) &&
                Test.TestTrue(TEXT("Pivot child attaches"),
                    PivotChild->AttachToComponent(Pivot, FAttachmentTransformRules::KeepWorldTransform));
        }
    };
}

#define ORIGIN_PARITY_TEST(Class, Path) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Path, \
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_PARITY_TEST(FOriginParityBlueprintSurface, "Origin.Runtime.Parity.ReflectedBlueprintAPI")
bool FOriginParityBlueprintSurface::RunTest(const FString&)
{
    // Both types must expose the same callable API, not just equivalent C++ helpers.
    for (const TCHAR* Name : {
        TEXT("GetOriginPivotSlotDefinition"), TEXT("CreateOriginPivotSlot"),
        TEXT("CaptureOriginPivotSlot"), TEXT("DuplicateOriginPivotSlot"),
        TEXT("RenameOriginPivotSlot"), TEXT("GetOriginPivotSlotNames"),
        TEXT("DeleteOriginPivotSlot"), TEXT("GetOriginPivotSlotPose"),
        TEXT("ActivateOriginPivotSlot"), TEXT("CanSetOriginPivotWorldTransform"),
        TEXT("SetOriginPivotWorldTransform") })
    {
        TestNotNull(*FString::Printf(TEXT("Anchor API: %s"), Name),
            AOriginAnchor::StaticClass()->FindFunctionByName(FName(Name)));
        TestNotNull(*FString::Printf(TEXT("Component API: %s"), Name),
            UOriginPivotComponent::StaticClass()->FindFunctionByName(FName(Name)));
    }
    TestNotNull(TEXT("Anchor pivot change event"),
        AOriginAnchor::StaticClass()->FindPropertyByName(TEXT("OnOriginPivotChanged")));
    TestNotNull(TEXT("Component pivot change event"),
        UOriginPivotComponent::StaticClass()->FindPropertyByName(TEXT("OnOriginPivotChanged")));
    return true;
}

ORIGIN_PARITY_TEST(FOriginParityNames, "Origin.Runtime.Parity.IdenticalSlotCRUD")
bool FOriginParityNames::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Both start with Default slot"),
        F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Default")) != nullptr &&
        F.Pivot->OriginPivotConfiguration.FindSlot(TEXT("Default")) != nullptr);
    // Out-params require mutable storage, not a temporary FTransform.
    FTransform Uncaptured;
    TestFalse(TEXT("Anchor Default has no invented pose"),
        F.Anchor->GetOriginPivotSlotPose(TEXT("Default"), Uncaptured));
    TestFalse(TEXT("Component Default has no invented pose"),
        F.Pivot->GetOriginPivotSlotPose(TEXT("Default"), Uncaptured));
    TestTrue(TEXT("Anchor create"), F.Anchor->CreateOriginPivotSlot(TEXT("Hinge"), Reason));
    TestTrue(TEXT("Component create"), F.Pivot->CreateOriginPivotSlot(TEXT("Hinge"), Reason));
    TestFalse(TEXT("Anchor duplicate name rejected"), F.Anchor->CreateOriginPivotSlot(TEXT("Hinge"), Reason));
    TestFalse(TEXT("Component duplicate name rejected"), F.Pivot->CreateOriginPivotSlot(TEXT("Hinge"), Reason));
    TestTrue(TEXT("Anchor duplicate"), F.Anchor->DuplicateOriginPivotSlot(TEXT("Hinge"), TEXT("Copy"), Reason));
    TestTrue(TEXT("Component duplicate"), F.Pivot->DuplicateOriginPivotSlot(TEXT("Hinge"), TEXT("Copy"), Reason));
    TestTrue(TEXT("Anchor rename"), F.Anchor->RenameOriginPivotSlot(TEXT("Copy"), TEXT("Renamed"), Reason));
    TestTrue(TEXT("Component rename"), F.Pivot->RenameOriginPivotSlot(TEXT("Copy"), TEXT("Renamed"), Reason));
    TestFalse(TEXT("Anchor cannot rename Default"), F.Anchor->RenameOriginPivotSlot(TEXT("Default"), TEXT("Other"), Reason));
    TestFalse(TEXT("Component cannot rename Default"), F.Pivot->RenameOriginPivotSlot(TEXT("Default"), TEXT("Other"), Reason));
    TestTrue(TEXT("Anchor delete inactive"), F.Anchor->DeleteOriginPivotSlot(TEXT("Renamed"), Reason));
    TestTrue(TEXT("Component delete inactive"), F.Pivot->DeleteOriginPivotSlot(TEXT("Renamed"), Reason));
    TestTrue(TEXT("Anchor deletes active named slot"), F.Anchor->DeleteOriginPivotSlot(TEXT("Hinge"), Reason));
    TestTrue(TEXT("Component deletes active named slot"), F.Pivot->DeleteOriginPivotSlot(TEXT("Hinge"), Reason));
    TArray<FName> AnchorNames, ComponentNames;
    F.Anchor->GetOriginPivotSlotNames(AnchorNames);
    F.Pivot->GetOriginPivotSlotNames(ComponentNames);
    TestTrue(TEXT("Identical slot names/order"), AnchorNames == ComponentNames);
    TestEqual(TEXT("Both have only Default"), AnchorNames.Num(), 1);
    TestEqual(TEXT("Anchor active Default"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestEqual(TEXT("Component active Default"), F.Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestEqual(TEXT("Anchor slot data promoted to schema 2"), F.Anchor->OriginPivotConfiguration.SchemaVersion, 2);
    return true;
}

// Deleting the currently active named pose must allow a true Default-only state
// on both hosts, without activating Default or moving attached geometry.
ORIGIN_PARITY_TEST(FOriginParityDeleteActive, "Origin.Runtime.Parity.DeleteActiveSlotReturnsToDefault")
bool FOriginParityDeleteActive::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    const FTransform AnchorBefore = F.Anchor->GetActorTransform();
    const FTransform ActorBefore = F.AnchorChild->GetActorTransform();
    const FTransform PivotBefore = F.Pivot->GetComponentTransform();
    const FTransform ComponentActorBefore = F.PivotChild->GetActorTransform();
    if (!TestTrue(TEXT("Anchor creates active named pose"),
        F.Anchor->CreateOriginPivotSlot(TEXT("Temporary"), Reason)) ||
        !TestTrue(TEXT("Component creates active named pose"),
        F.Pivot->CreateOriginPivotSlot(TEXT("Temporary"), Reason))) return false;
    TestEqual(TEXT("Anchor active before delete"),
        F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Temporary")));
    TestEqual(TEXT("Component active before delete"),
        F.Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Temporary")));
    if (!TestTrue(TEXT("Anchor can delete active pose"),
        F.Anchor->DeleteOriginPivotSlot(TEXT("Temporary"), Reason)) ||
        !TestTrue(TEXT("Component can delete active pose"),
        F.Pivot->DeleteOriginPivotSlot(TEXT("Temporary"), Reason))) return false;
    TArray<FName> AnchorNames, ComponentNames;
    F.Anchor->GetOriginPivotSlotNames(AnchorNames);
    F.Pivot->GetOriginPivotSlotNames(ComponentNames);
    TestEqual(TEXT("Anchor has only Default"), AnchorNames.Num(), 1);
    TestEqual(TEXT("Component has only Default"), ComponentNames.Num(), 1);
    TestTrue(TEXT("Both retained the required Default slot"),
        AnchorNames.Contains(FName(TEXT("Default"))) && ComponentNames.Contains(FName(TEXT("Default"))));
    FTransform DefaultPose;
    TestFalse(TEXT("Anchor Default remains uncaptured after deletion"),
        F.Anchor->GetOriginPivotSlotPose(TEXT("Default"), DefaultPose));
    TestFalse(TEXT("Component Default remains uncaptured after deletion"),
        F.Pivot->GetOriginPivotSlotPose(TEXT("Default"), DefaultPose));
    TestEqual(TEXT("Anchor active resets to Default"),
        F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestEqual(TEXT("Component active resets to Default"),
        F.Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    TestTrue(TEXT("Deleting Anchor slot never moves pivot"), Same(F.Anchor->GetActorTransform(), AnchorBefore));
    TestTrue(TEXT("Deleting Anchor slot never moves Actor"), Same(F.AnchorChild->GetActorTransform(), ActorBefore));
    TestTrue(TEXT("Deleting component slot never moves pivot"), Same(F.Pivot->GetComponentTransform(), PivotBefore));
    TestTrue(TEXT("Deleting component slot never moves Actor"), Same(F.PivotChild->GetActorTransform(), ComponentActorBefore));
    TestFalse(TEXT("Anchor rejects Default deletion"), F.Anchor->DeleteOriginPivotSlot(TEXT("Default"), Reason));
    TestFalse(TEXT("Component rejects Default deletion"), F.Pivot->DeleteOriginPivotSlot(TEXT("Default"), Reason));
    TestEqual(TEXT("Rejected Anchor Default delete changes nothing"), F.Anchor->OriginPivotConfiguration.Slots.Num(), 1);
    TestEqual(TEXT("Rejected component Default delete changes nothing"), F.Pivot->OriginPivotConfiguration.Slots.Num(), 1);
    TestTrue(TEXT("Anchor can create a new named pose again"), F.Anchor->CreateOriginPivotSlot(TEXT("Again"), Reason));
    TestTrue(TEXT("Component can create a new named pose again"), F.Pivot->CreateOriginPivotSlot(TEXT("Again"), Reason));
    TestTrue(TEXT("Anchor can return to Default-only again"), F.Anchor->DeleteOriginPivotSlot(TEXT("Again"), Reason));
    TestTrue(TEXT("Component can return to Default-only again"), F.Pivot->DeleteOriginPivotSlot(TEXT("Again"), Reason));
    return true;
}

ORIGIN_PARITY_TEST(FOriginParityPreservation, "Origin.Runtime.Parity.CaptureActivatePreservesAttachedBranches")
bool FOriginParityPreservation::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Capture Anchor Default"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    TestTrue(TEXT("Capture component Default"), F.Pivot->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    const FTransform AnchorInitial = F.Anchor->GetActorTransform();
    const FTransform PivotInitial = F.Pivot->GetComponentTransform();
    const FTransform ChildInitial = F.AnchorChild->GetActorTransform();
    const FTransform GrandInitial = F.AnchorGrandchild->GetActorTransform();
    const FTransform ComponentChildInitial = F.PivotChild->GetActorTransform();
    FTransform AnchorNext = AnchorInitial, PivotNext = PivotInitial;
    AnchorNext.AddToTranslation(FVector(42, -18, 9));
    PivotNext.AddToTranslation(FVector(42, -18, 9));
    TestTrue(TEXT("Anchor child-preserving pivot edit"),
        Accepted(F.Anchor->SetOriginPivotWorldTransform(AnchorNext, Reason)));
    TestTrue(TEXT("Component child-preserving pivot edit"),
        Accepted(F.Pivot->SetOriginPivotWorldTransform(PivotNext, Reason)));
    TestTrue(TEXT("Anchor child stays put"), Same(ChildInitial, F.AnchorChild->GetActorTransform()));
    TestTrue(TEXT("Anchor grandchild stays put"), Same(GrandInitial, F.AnchorGrandchild->GetActorTransform()));
    TestTrue(TEXT("Component attached Actor stays put"), Same(ComponentChildInitial, F.PivotChild->GetActorTransform()));
    TestTrue(TEXT("Anchor creates second pose"), F.Anchor->CreateOriginPivotSlot(TEXT("Second"), Reason));
    TestTrue(TEXT("Component creates second pose"), F.Pivot->CreateOriginPivotSlot(TEXT("Second"), Reason));
    TestTrue(TEXT("Anchor activates Default"), Accepted(F.Anchor->ActivateOriginPivotSlot(TEXT("Default"), Reason)));
    TestTrue(TEXT("Component activates Default"), Accepted(F.Pivot->ActivateOriginPivotSlot(TEXT("Default"), Reason)));
    TestTrue(TEXT("Anchor returns to saved pose"), Same(AnchorInitial, F.Anchor->GetActorTransform()));
    TestTrue(TEXT("Component returns to saved pose"), Same(PivotInitial, F.Pivot->GetComponentTransform()));
    TestTrue(TEXT("Nested actor preserved after slot activation"), Same(GrandInitial, F.AnchorGrandchild->GetActorTransform()));
    TestTrue(TEXT("Component actor preserved after slot activation"), Same(ComponentChildInitial, F.PivotChild->GetActorTransform()));
    TestTrue(TEXT("Anchor activates Second"), Accepted(F.Anchor->ActivateOriginPivotSlot(TEXT("Second"), Reason)));
    TestTrue(TEXT("Component activates Second"), Accepted(F.Pivot->ActivateOriginPivotSlot(TEXT("Second"), Reason)));
    TestTrue(TEXT("Anchor returns to second pose"), Same(AnchorNext, F.Anchor->GetActorTransform()));
    TestTrue(TEXT("Component returns to second pose"), Same(PivotNext, F.Pivot->GetComponentTransform()));
    return true;
}

ORIGIN_PARITY_TEST(FOriginParityParentFrame, "Origin.Runtime.Parity.AnchorSlotsFollowMovedParent")
bool FOriginParityParentFrame::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    AActor* Parent = F.SpawnSceneActor(FVector(100, 80, 0));
    if (!TestNotNull(TEXT("Parent"), Parent)) return false;
    if (!TestTrue(TEXT("Anchor attaches beneath parent"),
        F.Anchor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))) return false;
    FText Reason;
    TestTrue(TEXT("Capture attached Anchor slot"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    const FTransform SavedRelative = F.Anchor->GetRootComponent()->GetRelativeTransform();
    FTransform NewPose = F.Anchor->GetActorTransform();
    NewPose.AddToTranslation(FVector(40, 5, 0));
    if (!TestTrue(TEXT("Change pivot"), Accepted(F.Anchor->SetOriginPivotWorldTransform(NewPose, Reason))))
    { AddError(Reason.ToString()); return false; }
    Parent->SetActorLocation(FVector(530, -150, 0), false, nullptr, ETeleportType::TeleportPhysics);
    const FTransform ChildBeforeSwitch = F.AnchorChild->GetActorTransform();
    if (!TestTrue(TEXT("Activate relative slot after moving parent"),
        Accepted(F.Anchor->ActivateOriginPivotSlot(TEXT("Default"), Reason))))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Slot follows parent frame"),
        Same(F.Anchor->GetActorTransform(), SavedRelative * Parent->GetRootComponent()->GetComponentTransform()));
    TestTrue(TEXT("Attached actor does not move during activation"),
        Same(ChildBeforeSwitch, F.AnchorChild->GetActorTransform()));
    return true;
}

ORIGIN_PARITY_TEST(FOriginParityReparentedFrame, "Origin.Runtime.Parity.AnchorFrameMismatchRejectsAtomically")
bool FOriginParityReparentedFrame::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Capture world-root slot"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    AActor* Parent = F.SpawnSceneActor(FVector(900, 0, 0));
    if (!TestNotNull(TEXT("Parent"), Parent)) return false;
    if (!TestTrue(TEXT("Reattach Anchor to parent"),
        F.Anchor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))) return false;
    const FTransform Before = F.Anchor->GetActorTransform();
    const FName Active = F.Anchor->OriginPivotConfiguration.ActiveSlotName;
    TestTrue(TEXT("Root/parent frame mismatch rejected"),
        F.Anchor->ActivateOriginPivotSlot(TEXT("Default"), Reason) == EOriginPivotResult::InvalidPivot);
    TestTrue(TEXT("Rejection explains frame mismatch"), !Reason.IsEmpty());
    TestTrue(TEXT("No change to anchor transform"), Same(Before, F.Anchor->GetActorTransform()));
    TestEqual(TEXT("No active slot mutation"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, Active);
    // Restore a valid parent-relative capture, then simulate an attachment
    // socket change. Origin must never fall back to parent component origin.
    TestTrue(TEXT("Recapture in new parent frame"),
        F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    FOriginPivotSlot* Slot = F.Anchor->OriginPivotConfiguration.FindSlot(TEXT("Default"));
    if (!TestNotNull(TEXT("Captured slot"), Slot)) return false;
    Slot->CapturedSocketName = TEXT("RemovedSocket");
    TestTrue(TEXT("Changed socket frame is rejected"),
        F.Anchor->ActivateOriginPivotSlot(TEXT("Default"), Reason) == EOriginPivotResult::InvalidPivot);
    TestTrue(TEXT("Socket rejection keeps world transform"), Same(Before, F.Anchor->GetActorTransform()));
    return true;
}

ORIGIN_PARITY_TEST(FOriginParityMobility, "Origin.Runtime.Parity.StaticAnchorRejectsMovementAtomically")
bool FOriginParityMobility::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Capture before mobility change"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    const FTransform Before = F.Anchor->GetActorTransform();
    FTransform Other = Before; Other.AddToTranslation(FVector(90, 0, 0));
    TestTrue(TEXT("Create an alternative slot"), F.Anchor->CreateOriginPivotSlot(TEXT("Other"), Reason));
    F.Anchor->GetRootComponent()->SetMobility(EComponentMobility::Static);
    TestTrue(TEXT("Static Anchor cannot move with runtime API"),
        F.Anchor->SetOriginPivotWorldTransform(Other, Reason) == EOriginPivotResult::UnsupportedMobility);
    TestTrue(TEXT("No move after rejection"), Same(Before, F.Anchor->GetActorTransform()));
    F.Anchor->OriginPivotConfiguration.ActiveSlotName = TEXT("Other");
    TestTrue(TEXT("Static Anchor cannot activate a captured pose"),
        F.Anchor->ActivateOriginPivotSlot(TEXT("Default"), Reason) == EOriginPivotResult::UnsupportedMobility);
    TestEqual(TEXT("Failed activation leaves active name"),
        F.Anchor->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Other")));
    return true;
}

ORIGIN_PARITY_TEST(FOriginParityUncaptured, "Origin.Runtime.Parity.UncapturedSlotsDoNotMoveActors")
bool FOriginParityUncaptured::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    const FTransform A = F.Anchor->GetActorTransform(), B = F.Pivot->GetComponentTransform();
    const FTransform Child = F.AnchorChild->GetActorTransform();
    const FName AActive = F.Anchor->OriginPivotConfiguration.ActiveSlotName;
    const FName BActive = F.Pivot->OriginPivotConfiguration.ActiveSlotName;
    FText Reason;
    TestTrue(TEXT("Anchor uncaptured slot activation rejects"),
        F.Anchor->ActivateOriginPivotSlot(TEXT("Default"), Reason) == EOriginPivotResult::InvalidTransform);
    TestTrue(TEXT("Component uncaptured slot activation rejects"),
        F.Pivot->ActivateOriginPivotSlot(TEXT("Default"), Reason) == EOriginPivotResult::InvalidTransform);
    TestTrue(TEXT("Anchor unchanged"), Same(A, F.Anchor->GetActorTransform()));
    TestTrue(TEXT("Component unchanged"), Same(B, F.Pivot->GetComponentTransform()));
    TestTrue(TEXT("Anchor child unchanged"), Same(Child, F.AnchorChild->GetActorTransform()));
    TestEqual(TEXT("Anchor slot name unchanged"), F.Anchor->OriginPivotConfiguration.ActiveSlotName, AActive);
    TestEqual(TEXT("Component slot name unchanged"), F.Pivot->OriginPivotConfiguration.ActiveSlotName, BActive);
    return true;
}

ORIGIN_PARITY_TEST(FOriginParitySlotsNoImplicitWrites, "Origin.Runtime.Parity.MovementDoesNotOverwriteSavedSlots")
bool FOriginParitySlotsNoImplicitWrites::RunTest(const FString&)
{
    FParityFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Capture Anchor Default"), F.Anchor->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    TestTrue(TEXT("Capture component Default"), F.Pivot->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    FTransform AnchorSaved, ComponentSaved;
    TestTrue(TEXT("Read Anchor saved pose"), F.Anchor->GetOriginPivotSlotPose(TEXT("Default"), AnchorSaved));
    TestTrue(TEXT("Read component saved pose"), F.Pivot->GetOriginPivotSlotPose(TEXT("Default"), ComponentSaved));
    FTransform AnchorTarget = F.Anchor->GetActorTransform();
    AnchorTarget.AddToTranslation(FVector(25, 10, 0));
    FTransform PivotTarget = F.Pivot->GetComponentTransform();
    PivotTarget.AddToTranslation(FVector(25, 10, 0));
    TestTrue(TEXT("Anchor edit"), Accepted(F.Anchor->SetOriginPivotWorldTransform(AnchorTarget, Reason)));
    TestTrue(TEXT("Component edit"), Accepted(F.Pivot->SetOriginPivotWorldTransform(PivotTarget, Reason)));
    FTransform AnchorStillSaved, ComponentStillSaved;
    TestTrue(TEXT("Read Anchor pose again"), F.Anchor->GetOriginPivotSlotPose(TEXT("Default"), AnchorStillSaved));
    TestTrue(TEXT("Read component pose again"), F.Pivot->GetOriginPivotSlotPose(TEXT("Default"), ComponentStillSaved));
    TestTrue(TEXT("Anchor pose not implicitly overwritten"), Same(AnchorSaved, AnchorStillSaved));
    TestTrue(TEXT("Component pose not implicitly overwritten"), Same(ComponentSaved, ComponentStillSaved));
    FOriginPivotSlot* ComponentSlot = F.Pivot->OriginPivotConfiguration.FindSlot(TEXT("Default"));
    if (!TestNotNull(TEXT("Component slot"), ComponentSlot)) return false;
    ComponentSlot->CapturedSocketName = TEXT("RemovedSocket");
    const FTransform BeforeRejected = F.Pivot->GetComponentTransform();
    TestTrue(TEXT("Component missing socket is rejected"),
        F.Pivot->ActivateOriginPivotSlot(TEXT("Default"), Reason) == EOriginPivotResult::InvalidPivot);
    TestTrue(TEXT("Component socket rejection is atomic"), Same(BeforeRejected, F.Pivot->GetComponentTransform()));
    return true;
}

#undef ORIGIN_PARITY_TEST
#endif
