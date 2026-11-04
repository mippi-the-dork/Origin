// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Deterministic Origin runtime/attachment regressions. Each case owns an isolated,
// temporary test world. No editor map, selected actor, or user asset is modified.

#include "OriginPivotComponent.h"
#include "OriginPivotCore.h"
#include "OriginPivotEditSession.h"
#include "OriginPivotViewportMode.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace OriginAutomation
{
    constexpr double LocationToleranceCm = 0.01;
    constexpr double RotationTolerance = 1.e-4;
    constexpr double ScaleTolerance = 1.e-4;

    bool SameTransform(const FTransform& A, const FTransform& B)
    {
        return A.GetLocation().Equals(B.GetLocation(), LocationToleranceCm)
            && A.GetRotation().Equals(B.GetRotation(), RotationTolerance)
            && A.GetScale3D().Equals(B.GetScale3D(), ScaleTolerance);
    }

    struct FFixture
    {
        FTestWorldWrapper WorldWrapper;
        UWorld* World = nullptr;
        AActor* Owner = nullptr;
        USceneComponent* Root = nullptr;
        UOriginPivotComponent* Pivot = nullptr;

        bool Initialize(FAutomationTestBase& Test)
        {
            if (!Test.TestTrue(TEXT("Isolated test world creation"),
                WorldWrapper.CreateTestWorld(EWorldType::Game))) return false;
            World = WorldWrapper.GetTestWorld();
            if (!Test.TestNotNull(TEXT("Isolated test world"), World)) return false;

            Owner = SpawnActor();
            if (!Test.TestNotNull(TEXT("Fixture Actor"), Owner)) return false;
            Root = AddComponent<USceneComponent>(Owner, TEXT("OriginTestRoot"), nullptr, FVector::ZeroVector);
            if (!Test.TestNotNull(TEXT("Fixture root"), Root)) return false;
            Pivot = AddComponent<UOriginPivotComponent>(Owner, TEXT("OriginTestPivot"), Root, FVector(50, 30, 10));
            return Test.TestNotNull(TEXT("Origin Pivot"), Pivot)
                && Test.TestTrue(TEXT("Pivot registered"), Pivot->IsRegistered());
        }

        AActor* SpawnActor()
        {
            if (!World) return nullptr;
            FActorSpawnParameters Parameters;
            Parameters.ObjectFlags |= RF_Transient;
            Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            return World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Parameters);
        }

        template <typename T>
        T* AddComponent(AActor* Actor, const FName Name, USceneComponent* Parent, const FVector& RelativeLocation)
        {
            if (!Actor) return nullptr;
            T* Component = NewObject<T>(Actor, Name, RF_Transient);
            if (!Component) return nullptr;
            Component->SetMobility(EComponentMobility::Movable);
            Actor->AddInstanceComponent(Component);
            if (Parent)
            {
                Component->SetupAttachment(Parent);
            }
            else
            {
                Actor->SetRootComponent(Component);
            }
            Component->RegisterComponent();
            Component->SetRelativeLocation(RelativeLocation);
            return Component;
        }
    };

    bool AssertResult(FAutomationTestBase& Test, EOriginPivotResult Actual,
        EOriginPivotResult Expected, const TCHAR* Context)
    {
        return Test.TestTrue(Context, Actual == Expected);
    }

    bool AssertPreserved(FAutomationTestBase& Test, USceneComponent* Component,
        const FTransform& Expected, const TCHAR* Context)
    {
        return Test.TestTrue(Context, Component && SameTransform(Component->GetComponentTransform(), Expected));
    }
}

#define ORIGIN_TEST(Class, Name) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Name, \
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_TEST(FOriginPivotTranslationRegression,
    "Origin.Runtime.Preservation.Translation")
bool FOriginPivotTranslationRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* ChildA = F.AddComponent<USceneComponent>(F.Owner, TEXT("MeshA"), F.Pivot, FVector(-200, 0, 0));
    USceneComponent* ChildB = F.AddComponent<USceneComponent>(F.Owner, TEXT("MeshB"), F.Pivot, FVector(0, 200, 0));
    if (!TestNotNull(TEXT("Child A"), ChildA) || !TestNotNull(TEXT("Child B"), ChildB)) return false;

    const FTransform BeforeA = ChildA->GetComponentTransform();
    const FTransform BeforeB = ChildB->GetComponentTransform();
    const FTransform BeforePivot = F.Pivot->GetComponentTransform();
    FTransform Target = BeforePivot;
    Target.AddToTranslation(FVector(1000, 0, 0));
    FText Reason;
    if (!OriginAutomation::AssertResult(*this, F.Pivot->CanSetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::Success, TEXT("Translation preflight succeeds"))) return false;
    TestTrue(TEXT("Preflight did not move pivot"), OriginAutomation::SameTransform(F.Pivot->GetComponentTransform(), BeforePivot));
    OriginAutomation::AssertResult(*this, F.Pivot->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::Success, TEXT("Translation succeeds"));
    OriginAutomation::AssertPreserved(*this, F.Pivot, Target, TEXT("Pivot moved to target"));
    OriginAutomation::AssertPreserved(*this, ChildA, BeforeA, TEXT("Child A world transform preserved"));
    OriginAutomation::AssertPreserved(*this, ChildB, BeforeB, TEXT("Child B world transform preserved"));
    return true;
}

ORIGIN_TEST(FOriginPivotRotationRegression,
    "Origin.Runtime.Preservation.Rotation")
bool FOriginPivotRotationRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("RotatingBranch"), F.Pivot, FVector(100, 40, 5));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    const FTransform ChildBefore = Child->GetComponentTransform();
    FTransform Target = F.Pivot->GetComponentTransform();
    Target.SetRotation(FRotator(10, 45, -12).Quaternion() * Target.GetRotation());
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::Success, TEXT("Rotation succeeds"));
    OriginAutomation::AssertPreserved(*this, F.Pivot, Target, TEXT("Pivot rotation applied"));
    OriginAutomation::AssertPreserved(*this, Child, ChildBefore, TEXT("Child location and rotation preserved"));
    return true;
}

ORIGIN_TEST(FOriginPivotNestedRegression,
    "Origin.Runtime.Preservation.NestedComponents")
bool FOriginPivotNestedRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* A = F.AddComponent<USceneComponent>(F.Owner, TEXT("NestedA"), F.Pivot, FVector(-100, 5, 0));
    UOriginPivotComponent* B = F.AddComponent<UOriginPivotComponent>(F.Owner, TEXT("NestedPivotB"), F.Pivot, FVector(150, 0, 30));
    USceneComponent* C = F.AddComponent<USceneComponent>(F.Owner, TEXT("NestedC"), B, FVector(75, 25, 0));
    USceneComponent* D = F.AddComponent<USceneComponent>(F.Owner, TEXT("NestedD"), C, FVector(10, -15, 40));
    if (!TestNotNull(TEXT("Nested A"), A) || !TestNotNull(TEXT("Nested pivot B"), B)
        || !TestNotNull(TEXT("Nested C"), C) || !TestNotNull(TEXT("Nested D"), D)) return false;

    const FTransform BeforeA = A->GetComponentTransform();
    const FTransform BeforeB = B->GetComponentTransform();
    const FTransform BeforeC = C->GetComponentTransform();
    const FTransform BeforeD = D->GetComponentTransform();
    FTransform Target = F.Pivot->GetComponentTransform();
    Target.AddToTranslation(FVector(70, -125, 33));
    Target.SetRotation(FRotator(0, 65, 0).Quaternion() * Target.GetRotation());
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::Success, TEXT("Parent pivot edit succeeds"));
    OriginAutomation::AssertPreserved(*this, A, BeforeA, TEXT("First branch retained"));
    OriginAutomation::AssertPreserved(*this, B, BeforeB, TEXT("Nested Origin Pivot retained"));
    OriginAutomation::AssertPreserved(*this, C, BeforeC, TEXT("Grandchild retained"));
    OriginAutomation::AssertPreserved(*this, D, BeforeD, TEXT("Great-grandchild retained"));

    Target = B->GetComponentTransform();
    Target.AddToTranslation(FVector(0, 0, 90));
    OriginAutomation::AssertResult(*this, B->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::Success, TEXT("Nested pivot edit succeeds"));
    OriginAutomation::AssertPreserved(*this, A, BeforeA, TEXT("Sibling retained after nested edit"));
    OriginAutomation::AssertPreserved(*this, C, BeforeC, TEXT("Grandchild retained after nested edit"));
    OriginAutomation::AssertPreserved(*this, D, BeforeD, TEXT("Great-grandchild retained after nested edit"));
    return true;
}

ORIGIN_TEST(FOriginPivotActorRegression,
    "Origin.Runtime.Preservation.AttachedActors")
bool FOriginPivotActorRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    AActor* AttachedActor = F.SpawnActor();
    if (!TestNotNull(TEXT("Attached Actor"), AttachedActor)) return false;
    USceneComponent* ExternalRoot = F.AddComponent<USceneComponent>(AttachedActor, TEXT("ExternalRoot"), nullptr, FVector::ZeroVector);
    USceneComponent* ExternalChild = F.AddComponent<USceneComponent>(AttachedActor, TEXT("ExternalChild"), ExternalRoot, FVector(12, 34, 56));
    if (!TestNotNull(TEXT("External root"), ExternalRoot) || !TestNotNull(TEXT("External child"), ExternalChild)) return false;
    AttachedActor->SetActorLocation(FVector(450, -110, 40));
    if (!TestTrue(TEXT("Attach Actor with Keep World"),
        AttachedActor->AttachToComponent(F.Pivot, FAttachmentTransformRules::KeepWorldTransform))) return false;
    TestTrue(TEXT("Actor root is attached to Origin"), ExternalRoot->GetAttachParent() == F.Pivot);
    const FTransform BeforeActor = AttachedActor->GetActorTransform();
    const FTransform BeforeRoot = ExternalRoot->GetComponentTransform();
    const FTransform BeforeChild = ExternalChild->GetComponentTransform();
    FTransform Target = F.Pivot->GetComponentTransform();
    Target.AddToTranslation(FVector(200, 100, -50));
    Target.SetRotation(FRotator(0, 40, 0).Quaternion());
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::Success, TEXT("Attached Actor preservation succeeds"));
    TestTrue(TEXT("Actor world transform preserved"), OriginAutomation::SameTransform(AttachedActor->GetActorTransform(), BeforeActor));
    OriginAutomation::AssertPreserved(*this, ExternalRoot, BeforeRoot, TEXT("Attached root preserved"));
    OriginAutomation::AssertPreserved(*this, ExternalChild, BeforeChild, TEXT("External child preserved"));
    // An ordinary, native move after authoring must still move the attached Actor.
    F.Pivot->AddWorldOffset(FVector(80, 0, 0));
    TestFalse(TEXT("Ordinary pivot movement moves the Actor"),
        OriginAutomation::SameTransform(AttachedActor->GetActorTransform(), BeforeActor));
    return true;
}

ORIGIN_TEST(FOriginPivotNoChangeRegression,
    "Origin.Runtime.Validation.NoChange")
bool FOriginPivotNoChangeRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("NoChangeChild"), F.Pivot, FVector(25, 0, 0));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    const FTransform Original = F.Pivot->GetComponentTransform();
    const FTransform OriginalChild = Child->GetComponentTransform();
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->CanSetOriginPivotWorldTransform(Original, Reason),
        EOriginPivotResult::NoChange, TEXT("Preflight reports NoChange"));
    OriginAutomation::AssertResult(*this, F.Pivot->SetOriginPivotWorldTransform(Original, Reason),
        EOriginPivotResult::NoChange, TEXT("Apply reports NoChange"));
    OriginAutomation::AssertPreserved(*this, F.Pivot, Original, TEXT("Pivot unchanged"));
    OriginAutomation::AssertPreserved(*this, Child, OriginalChild, TEXT("Child unchanged"));
    return true;
}

ORIGIN_TEST(FOriginPivotUnsupportedScaleRegression,
    "Origin.Runtime.Validation.RejectScaleChange")
bool FOriginPivotUnsupportedScaleRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("ScaleChild"), F.Pivot, FVector(45, 0, 0));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    const FTransform PivotBefore = F.Pivot->GetComponentTransform();
    const FTransform ChildBefore = Child->GetComponentTransform();
    FTransform Target = PivotBefore;
    Target.SetScale3D(FVector(2, 1, 1));
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::UnsupportedScale, TEXT("Scale change rejected"));
    TestFalse(TEXT("Rejection provides a reason"), Reason.IsEmpty());
    OriginAutomation::AssertPreserved(*this, F.Pivot, PivotBefore, TEXT("Pivot unchanged on rejection"));
    OriginAutomation::AssertPreserved(*this, Child, ChildBefore, TEXT("Child unchanged on rejection"));
    return true;
}

ORIGIN_TEST(FOriginPivotNonUniformAncestorRegression,
    "Origin.Runtime.Validation.RejectNonUniformAncestor")
bool FOriginPivotNonUniformAncestorRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    F.Root->SetWorldScale3D(FVector(2, 1, 1));
    const FTransform Original = F.Pivot->GetComponentTransform();
    FTransform Target = Original;
    Target.AddToTranslation(FVector(100, 0, 0));
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->CanSetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::UnsupportedScale, TEXT("Nonuniform scale rejected during preflight"));
    OriginAutomation::AssertPreserved(*this, F.Pivot, Original, TEXT("Rejected preflight never moves pivot"));
    return true;
}

ORIGIN_TEST(FOriginPivotUnregisteredRegression,
    "Origin.Runtime.Validation.RejectUnregistered")
bool FOriginPivotUnregisteredRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    UOriginPivotComponent* Unregistered = NewObject<UOriginPivotComponent>(F.Owner, TEXT("UnregisteredOriginPivot"), RF_Transient);
    if (!TestNotNull(TEXT("Unregistered pivot"), Unregistered)) return false;
    FText Reason;
    FTransform Target = FTransform::Identity;
    Target.SetLocation(FVector(100, 0, 0));
    OriginAutomation::AssertResult(*this, Unregistered->SetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::UnregisteredComponent, TEXT("Unregistered pivot rejected"));
    TestFalse(TEXT("Unregistered reason populated"), Reason.IsEmpty());
    return true;
}

ORIGIN_TEST(FOriginPivotInvalidTransformRegression,
    "Origin.Runtime.Validation.RejectInvalidTransform")
bool FOriginPivotInvalidTransformRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    const FTransform Original = F.Pivot->GetComponentTransform();
    FTransform Target = Original;
    Target.SetRotation(FQuat(0, 0, 0, 0));
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->CanSetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::InvalidTransform, TEXT("Non-normalized rotation rejected"));
    OriginAutomation::AssertPreserved(*this, F.Pivot, Original, TEXT("Invalid preflight never moves pivot"));
    return true;
}

ORIGIN_TEST(FOriginPivotStationaryRegression,
    "Origin.Runtime.Validation.RejectNonMovablePivot")
bool FOriginPivotStationaryRegression::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    F.Root->SetMobility(EComponentMobility::Static);
    F.Pivot->SetMobility(EComponentMobility::Stationary);
    const FTransform Original = F.Pivot->GetComponentTransform();
    FTransform Target = Original;
    Target.AddToTranslation(FVector(25, 0, 0));
    FText Reason;
    OriginAutomation::AssertResult(*this, F.Pivot->CanSetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::UnsupportedMobility, TEXT("Stationary pivot rejected"));
    OriginAutomation::AssertPreserved(*this, F.Pivot, Original, TEXT("Rejected pivot unchanged"));
    return true;
}


ORIGIN_TEST(FOriginPivotTemplateRegression,
    "Origin.Runtime.Validation.RejectTemplate")
bool FOriginPivotTemplateRegression::RunTest(const FString& Parameters)
{
    const UOriginPivotComponent* Template = GetDefault<UOriginPivotComponent>();
    if (!TestNotNull(TEXT("Origin Pivot class default object"), Template)) return false;
    FTransform Target = FTransform::Identity;
    Target.SetLocation(FVector(100, 0, 0));
    FText Reason;
    OriginAutomation::AssertResult(*this, Template->CanSetOriginPivotWorldTransform(Target, Reason),
        EOriginPivotResult::InvalidPivot, TEXT("Template cannot be edited"));
    TestFalse(TEXT("Rejection includes explanation"), Reason.IsEmpty());
    return true;
}

ORIGIN_TEST(FOriginPivotBlueprintContractRegression,
    "Origin.Runtime.Blueprint.APIContract")
bool FOriginPivotBlueprintContractRegression::RunTest(const FString& Parameters)
{
    const UClass* PivotClass = UOriginPivotComponent::StaticClass();
    if (!TestNotNull(TEXT("Origin Pivot class"), PivotClass)) return false;
    TestTrue(TEXT("Blueprint Add Component metadata exists"),
        PivotClass->HasMetaData(TEXT("BlueprintSpawnableComponent")));

    for (const FName FunctionName : {
        FName(TEXT("SetOriginPivotWorldTransform")),
        FName(TEXT("CanSetOriginPivotWorldTransform")),
        FName(TEXT("GetOriginPivotSlotDefinition"))})
    {
        const UFunction* Function = PivotClass->FindFunctionByName(FunctionName);
        if (!TestNotNull(*FString::Printf(TEXT("Blueprint node '%s'"), *FunctionName.ToString()), Function))
            continue;
        TestTrue(*FString::Printf(TEXT("'%s' is Blueprint callable"), *FunctionName.ToString()),
            Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
    }
    const FProperty* EventProperty = PivotClass->FindPropertyByName(TEXT("OnOriginPivotChanged"));
    if (TestNotNull(TEXT("On Origin Pivot Changed dispatcher"), EventProperty))
    {
        TestTrue(TEXT("Pivot changed dispatcher is Blueprint assignable"),
            EventProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable));
    }
    const UOriginPivotComponent* DefaultPivot = GetDefault<UOriginPivotComponent>();
    TestFalse(TEXT("Origin Pivot does not need Tick"), DefaultPivot->PrimaryComponentTick.bCanEverTick);
    return true;
}


// The production editor session is exercised against temporary game worlds.
// Global Undo recording is intentionally disabled in these fixtures so tests
// cannot add transactions to the user's current editor history.
ORIGIN_TEST(FOriginEditorSessionTranslationDrag,
    "Origin.Editor.Session.MultiDeltaPreservation")
bool FOriginEditorSessionTranslationDrag::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("EditorDragChild"), F.Pivot, FVector(40, -50, 12));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    const FTransform BeforeChild = Child->GetComponentTransform();
    const FTransform BeforePivot = F.Pivot->GetComponentTransform();
    FOriginPivotEditSession Session(F.Pivot, false);
    FText Reason;
    FTransform Target = BeforePivot;
    Target.AddToTranslation(FVector(25, 0, 0));
    OriginAutomation::AssertResult(*this, Session.Apply(Target, Reason), EOriginPivotResult::Success, TEXT("First drag delta succeeds"));
    Target = F.Pivot->GetComponentTransform();
    Target.AddToTranslation(FVector(0, 35, 0));
    Target.SetRotation(FRotator(0, 30, 0).Quaternion() * Target.GetRotation());
    OriginAutomation::AssertResult(*this, Session.Apply(Target, Reason), EOriginPivotResult::Success, TEXT("Second drag delta succeeds"));
    TestTrue(TEXT("Session records a change"), Session.HasChanged());
    OriginAutomation::AssertPreserved(*this, Child, BeforeChild, TEXT("Child remains stationary after multiple deltas"));
    Session.Finish();
    OriginAutomation::AssertPreserved(*this, F.Pivot, Target, TEXT("Finishing drag keeps pivot"));
    OriginAutomation::AssertPreserved(*this, Child, BeforeChild, TEXT("Finishing drag keeps child"));
    return true;
}

ORIGIN_TEST(FOriginEditorSessionCancelDrag,
    "Origin.Editor.Session.CancelRestoresAssembly")
bool FOriginEditorSessionCancelDrag::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("EditorCancelChild"), F.Pivot, FVector(120, 10, 0));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    const FTransform BeforeChild = Child->GetComponentTransform();
    const FTransform BeforePivot = F.Pivot->GetComponentTransform();
    FOriginPivotEditSession Session(F.Pivot, false);
    FText Reason;
    FTransform Target = BeforePivot;
    Target.AddToTranslation(FVector(200, -40, 70));
    Target.SetRotation(FRotator(15, 60, -5).Quaternion() * Target.GetRotation());
    OriginAutomation::AssertResult(*this, Session.Apply(Target, Reason), EOriginPivotResult::Success, TEXT("Preview move succeeds"));
    OriginAutomation::AssertPreserved(*this, Child, BeforeChild, TEXT("Preview preserves child"));
    Session.Cancel();
    TestFalse(TEXT("Cancel resets changed state"), Session.HasChanged());
    OriginAutomation::AssertPreserved(*this, F.Pivot, BeforePivot, TEXT("Cancel restores pivot"));
    OriginAutomation::AssertPreserved(*this, Child, BeforeChild, TEXT("Cancel restores child"));
    return true;
}

ORIGIN_TEST(FOriginEditorSessionUnsafeDrag,
    "Origin.Editor.Session.RejectedEditIsAtomic")
bool FOriginEditorSessionUnsafeDrag::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("EditorRejectChild"), F.Pivot, FVector(100, 0, 0));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    const FTransform BeforePivot = F.Pivot->GetComponentTransform();
    const FTransform BeforeChild = Child->GetComponentTransform();
    FOriginPivotEditSession Session(F.Pivot, false);
    FText Reason;
    FTransform Target = BeforePivot;
    Target.SetScale3D(FVector(2, 1, 1));
    OriginAutomation::AssertResult(*this, Session.Apply(Target, Reason), EOriginPivotResult::UnsupportedScale, TEXT("Unsafe scale rejected"));
    TestFalse(TEXT("Rejected change never begins drag"), Session.HasChanged());
    TestFalse(TEXT("Reject provides explanation"), Reason.IsEmpty());
    OriginAutomation::AssertPreserved(*this, F.Pivot, BeforePivot, TEXT("Pivot unchanged"));
    OriginAutomation::AssertPreserved(*this, Child, BeforeChild, TEXT("Child unchanged"));
    Session.Finish();
    return true;
}

// A widget drag is already committed as it happens; Details must display the
// world-space offset since mode activation, not add the same offset a second time.
// Math-only regression: does not activate editor modes or alter global selection.
ORIGIN_TEST(FOriginViewportDisplayedDeltaRegression,
    "Origin.Editor.Viewport.LiveOffsetComputation")
bool FOriginViewportDisplayedDeltaRegression::RunTest(const FString& Parameters)
{
    const FTransform StartingWorld(FRotator(0, 20, 0), FVector(50, -60, 100), FVector::OneVector);
    FTransform EditedWorld = StartingWorld;
    EditedWorld.AddToTranslation(FVector(120, -25, 10));
    EditedWorld.SetRotation(FRotator(0, 45, 0).Quaternion() * StartingWorld.GetRotation());

    FVector Translation = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FOriginPivotViewportMode::CalculateEditDeltas(StartingWorld, EditedWorld, Translation, Rotation);
    TestTrue(TEXT("Translation reports the total already-applied world offset"),
        Translation.Equals(FVector(120, -25, 10), 0.001));
    TestTrue(TEXT("Rotation reports the applied world yaw, not absolute yaw"),
        FMath::IsNearlyEqual(Rotation.Yaw, 45.0, 0.001));
    TestTrue(TEXT("Rotation has no unintended pitch or roll"),
        FMath::IsNearlyZero(Rotation.Pitch, 0.001) && FMath::IsNearlyZero(Rotation.Roll, 0.001));
    return true;
}

#undef ORIGIN_TEST
#endif // WITH_DEV_AUTOMATION_TESTS


#if WITH_DEV_AUTOMATION_TESTS
#include "OriginPivotSnapMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginSlotSwitchPreservesAssemblyTest,
    "Origin.Runtime.Slots.ActivatePreservesAssembly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOriginSlotSwitchPreservesAssemblyTest::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("SlotChild"),
        F.Pivot, FVector(150.0, -30.0, 20.0));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    FText Reason;
    TestTrue(TEXT("Capture Default"), F.Pivot->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    const FTransform OriginalPivot = F.Pivot->GetComponentTransform();
    FTransform DifferentPivot = OriginalPivot;
    DifferentPivot.AddToTranslation(FVector(100.0, 70.0, 50.0));
    DifferentPivot.SetRotation(FRotator(0, 65, 0).Quaternion());
    TestTrue(TEXT("Move for second pose"), F.Pivot->SetOriginPivotWorldTransform(DifferentPivot, Reason)
        == EOriginPivotResult::Success);
    TestTrue(TEXT("Create alternate pose"), F.Pivot->CreateOriginPivotSlot(TEXT("Other"), Reason));
    const FTransform ChildBeforeSwitch = Child->GetComponentTransform();
    TestTrue(TEXT("Activate Default"), F.Pivot->ActivateOriginPivotSlot(TEXT("Default"), Reason)
        == EOriginPivotResult::Success);
    TestTrue(TEXT("Restored original pivot"), OriginAutomation::SameTransform(OriginalPivot, F.Pivot->GetComponentTransform()));
    TestTrue(TEXT("Child remained stationary"), OriginAutomation::SameTransform(ChildBeforeSwitch, Child->GetComponentTransform()));
    TestEqual(TEXT("Active name"), F.Pivot->OriginPivotConfiguration.ActiveSlotName, FName(TEXT("Default")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginSlotRelativeParentTest,
    "Origin.Runtime.Slots.RelativeToMovedParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOriginSlotRelativeParentTest::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    USceneComponent* Child = F.AddComponent<USceneComponent>(F.Owner, TEXT("MovingChild"),
        F.Pivot, FVector(-20.0, 40.0, 50.0));
    if (!TestNotNull(TEXT("Child"), Child)) return false;
    FText Reason;
    TestTrue(TEXT("Capture Default relative pose"), F.Pivot->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    FTransform StoredPose;
    TestTrue(TEXT("Can read captured pose"), F.Pivot->GetOriginPivotSlotPose(TEXT("Default"), StoredPose));
    F.Root->SetWorldLocationAndRotation(FVector(300, -50, 200), FRotator(0, 90, 0));
    FTransform Offset = F.Pivot->GetComponentTransform();
    Offset.AddToTranslation(FVector(0, 60, 0));
    TestTrue(TEXT("Move pivot before reactivating"), F.Pivot->SetOriginPivotWorldTransform(Offset, Reason)
        == EOriginPivotResult::Success);
    const FTransform ChildBeforeSwitch = Child->GetComponentTransform();
    TestTrue(TEXT("Activate slot after moving whole assembly"), F.Pivot->ActivateOriginPivotSlot(TEXT("Default"), Reason)
        == EOriginPivotResult::Success);
    TestTrue(TEXT("Pose follows the parent"), OriginAutomation::SameTransform(
        StoredPose * F.Root->GetComponentTransform(), F.Pivot->GetComponentTransform()));
    TestTrue(TEXT("Child stays at the current world transform"), OriginAutomation::SameTransform(
        ChildBeforeSwitch, Child->GetComponentTransform()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginSlotAtomicRejectionTest,
    "Origin.Runtime.Slots.RejectedActivationIsAtomic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOriginSlotAtomicRejectionTest::RunTest(const FString& Parameters)
{
    OriginAutomation::FFixture F;
    if (!F.Initialize(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Capture invalid candidate fixture slot"), F.Pivot->CreateOriginPivotSlot(TEXT("Other"), Reason));
    TestTrue(TEXT("Make Default active before rejected switch"),
        F.Pivot->CaptureOriginPivotSlot(TEXT("Default"), Reason));
    FOriginPivotSlot* Other = F.Pivot->OriginPivotConfiguration.FindSlot(TEXT("Other"));
    if (!TestNotNull(TEXT("Other slot"), Other)) return false;
    Other->SavedRelativeTransform.SetScale3D(FVector(2, 1, 1));
    const FTransform Before = F.Pivot->GetComponentTransform();
    const FName ActiveBefore = F.Pivot->OriginPivotConfiguration.ActiveSlotName;
    TestTrue(TEXT("Unsupported scale rejected"), F.Pivot->ActivateOriginPivotSlot(TEXT("Other"), Reason)
        == EOriginPivotResult::UnsupportedScale);
    TestTrue(TEXT("Rejected activation did not move component"), OriginAutomation::SameTransform(
        Before, F.Pivot->GetComponentTransform()));
    TestEqual(TEXT("Rejected activation did not change active name"),
        F.Pivot->OriginPivotConfiguration.ActiveSlotName, ActiveBefore);
    TestFalse(TEXT("Duplicate slot cannot be created"), F.Pivot->CreateOriginPivotSlot(TEXT("Other"), Reason));
    TestFalse(TEXT("Default active slot cannot be deleted"),
        F.Pivot->DeleteOriginPivotSlot(TEXT("Default"), Reason));
    TestTrue(TEXT("Inactive invalid-pose slot can be deleted without activating it"),
        F.Pivot->DeleteOriginPivotSlot(TEXT("Other"), Reason));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginSlotSnapCenterMathTest,
    "Origin.Runtime.Slots.SnapAverageMath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOriginSlotSnapCenterMathTest::RunTest(const FString& Parameters)
{
    TArray<FTransform> Children;
    Children.Add(FTransform(FRotator::ZeroRotator, FVector(100, 0, 0)));
    Children.Add(FTransform(FRotator::ZeroRotator, FVector(-100, 0, 0)));
    const FTransform Pivot(FRotator(0, 90, 0), FVector(50, 60, 70));
    FVector Center(123, 456, 789);
    TestTrue(TEXT("Centered child origins valid"), FOriginPivotSnapMath::AverageChildOrigins(Pivot, Children, Center));
    TestTrue(TEXT("Rotation and translation applied to child origins"), Center.Equals(FVector(50, 60, 70), 1.e-4));
    Children.Reset();
    TestFalse(TEXT("Zero children rejected"), FOriginPivotSnapMath::AverageChildOrigins(Pivot, Children, Center));
    return true;
}
#endif
