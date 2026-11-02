// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Origin-only Unreal native Automation Framework tests. No project assets are modified.
#include "OriginBlueprintPivotEdit.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "OriginPivotViewportMode.h"
#include "OriginPivotComponent.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/PlatformTime.h"

namespace
{
    // FKismetEditorUtilities initializes UBlueprint, its generated class, and
    // SCS correctly. Never NewObject<UBlueprint>() and manually install SCS:
    // that previous test fixture was capable of crashing the editor.
    struct FTransientBlueprintFixture
    {
        TStrongObjectPtr<UBlueprint> Blueprint;
        bool Build(FAutomationTestBase& Test, int32 OriginPivotCount)
        {
            const FName UniqueName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(),
                FName(*FString::Printf(TEXT("OriginReconstruction%d"), OriginPivotCount)));
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
                GetTransientPackage(), UniqueName, BPTYPE_Normal, FName(TEXT("OriginNativeAutomation")));
            Blueprint.Reset(BP);
            if (!Test.TestNotNull(TEXT("Engine-created transient Actor Blueprint"), BP)) return false;
            USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
            if (!Test.TestNotNull(TEXT("Initialized Blueprint SCS"), SCS)) return false;

            USCS_Node* Root = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("TestRoot"));
            if (!Test.TestNotNull(TEXT("Scene root SCS node"), Root)) return false;
            SCS->AddNode(Root);
            USCS_Node* Parent = Root;
            // Equal topology for 0, 1, and 2 pivots. Only component class changes.
            for (int32 Index = 0; Index < 2; ++Index)
            {
                const bool bPivot = Index < OriginPivotCount;
                USCS_Node* Branch = SCS->CreateNode(
                    bPivot ? UOriginPivotComponent::StaticClass() : USceneComponent::StaticClass(),
                    FName(*FString::Printf(TEXT("Branch%d"), Index)));
                if (!Test.TestNotNull(TEXT("Branch SCS node"), Branch)) return false;
                Parent->AddChildNode(Branch, true);
                Parent = Branch;
            }
            for (int32 Index = 0; Index < 5; ++Index)
            {
                USCS_Node* Child = SCS->CreateNode(USceneComponent::StaticClass(),
                    FName(*FString::Printf(TEXT("Leaf%d"), Index)));
                if (!Test.TestNotNull(TEXT("Leaf SCS node"), Child)) return false;
                Parent->AddChildNode(Child, true);
            }
            FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
            // TestNotNull requires a raw pointer, not TSubclassOf<UObject>.
            return Test.TestNotNull(TEXT("Compiled Blueprint generated class"), BP->GeneratedClass.Get());
        }
    };

    double MeasureReconstruction(AActor* Actor, int32 Iterations)
    {
        for (int32 Index = 0; Index < 3; ++Index) Actor->RerunConstructionScripts();
        const double Start = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Iterations; ++Index) Actor->RerunConstructionScripts();
        return (FPlatformTime::Seconds() - Start) * 1000.0 / Iterations;
    }
}

#define ORIGIN_RECONSTRUCTION_TEST(Class, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_RECONSTRUCTION_TEST(FOriginBlueprintValidFixtureTest, "Origin.Performance.ConstructionScript.TransientBlueprintFixture")
bool FOriginBlueprintValidFixtureTest::RunTest(const FString& Parameters)
{
    FTransientBlueprintFixture Fixture;
    if (!Fixture.Build(*this, 1)) return false;
    FTestWorldWrapper TestWorld;
    if (!TestTrue(TEXT("Create isolated world for compiled transient Blueprint"),
        TestWorld.CreateTestWorld(EWorldType::Game))) return false;
    UWorld* World = TestWorld.GetTestWorld();
    if (!TestNotNull(TEXT("Fixture world"), World)) return false;
    FActorSpawnParameters Params;
    Params.ObjectFlags |= RF_Transient;
    AActor* Actor = World->SpawnActor<AActor>(Fixture.Blueprint->GeneratedClass,
        FVector::ZeroVector, FRotator::ZeroRotator, Params);
    if (!TestNotNull(TEXT("Transient Blueprint Actor instance"), Actor)) return false;
    Actor->RerunConstructionScripts();
    TArray<UOriginPivotComponent*> Pivots;
    Actor->GetComponents<UOriginPivotComponent>(Pivots);
    TestEqual(TEXT("Origin component created by actual Blueprint SCS"), Pivots.Num(), 1);
    World->DestroyActor(Actor);
    return true;
}

ORIGIN_RECONSTRUCTION_TEST(FOriginBlueprintReconstructionComparison, "Origin.Performance.ConstructionScript.NativeVsOrigin")
bool FOriginBlueprintReconstructionComparison::RunTest(const FString& Parameters)
{
    FTestWorldWrapper TestWorld;
    if (!TestTrue(TEXT("Create isolated world"), TestWorld.CreateTestWorld(EWorldType::Game))) return false;
    UWorld* World = TestWorld.GetTestWorld();
    if (!TestNotNull(TEXT("Isolated test world"), World)) return false;
    constexpr int32 Iterations = 30;
    double Ms[3] = {0.0, 0.0, 0.0};
    for (int32 Count = 0; Count <= 2; ++Count)
    {
        FTransientBlueprintFixture Fixture;
        if (!Fixture.Build(*this, Count)) return false;
        UClass* Generated = Fixture.Blueprint->GeneratedClass;
        if (!TestNotNull(TEXT("Generated Blueprint class"), Generated)) return false;
        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transient;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Instance = World->SpawnActor<AActor>(Generated, FVector::ZeroVector,
            FRotator::ZeroRotator, Params);
        if (!TestNotNull(TEXT("Spawn generated Blueprint Actor"), Instance)) return false;
        TArray<UOriginPivotComponent*> Pivots;
        Instance->GetComponents<UOriginPivotComponent>(Pivots);
        TestEqual(TEXT("Correct number of Origin pivots in generated Actor"), Pivots.Num(), Count);
        if (Pivots.Num() != Count) return false;
        Ms[Count] = MeasureReconstruction(Instance, Iterations);
        if (!TestTrue(TEXT("Reconstruction measurement is finite and nonnegative"),
                FMath::IsFinite(Ms[Count]) && Ms[Count] >= 0.0)) return false;
        World->DestroyActor(Instance);
    }
    AddInfo(FString::Printf(TEXT("Origin Blueprint reconstruction: same 8-node SCS hierarchy, %d RerunConstructionScripts per case. Native=%.4f ms, 1 Pivot=%.4f ms (%.2fx), 2 Pivots=%.4f ms (%.2fx). CPU-only isolated synthetic measurement, NOT viewport FPS, Blueprint user Construction Script graph, or rendering."),
        Iterations, Ms[0], Ms[1], Ms[0] > 0.0 ? Ms[1]/Ms[0] : 0.0,
        Ms[2], Ms[0] > 0.0 ? Ms[2]/Ms[0] : 0.0));
    return true; // timings are diagnostics, not a brittle performance threshold
}

ORIGIN_RECONSTRUCTION_TEST(FOriginBlueprintNativeDragAxisTest, "Origin.Editor.Blueprint.Viewport.NativeGizmoWithoutLegacyAxis")
bool FOriginBlueprintNativeDragAxisTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Blueprint gizmo drag can begin without legacy widget axis"),
        FOriginPivotViewportMode::CanStartTracking(true, false));
    TestTrue(TEXT("Legacy Blueprint widget axis remains supported"),
        FOriginPivotViewportMode::CanStartTracking(true, true));
    TestTrue(TEXT("Level Editor requires native widget axis"),
        FOriginPivotViewportMode::CanStartTracking(false, true));
    TestFalse(TEXT("Level Editor must not capture ordinary camera navigation"),
        FOriginPivotViewportMode::CanStartTracking(false, false));
    return true;
}

ORIGIN_RECONSTRUCTION_TEST(FOriginBlueprintPreviewCompensationTest, "Origin.Editor.Blueprint.Viewport.NativePreviewChildReconciliation")
bool FOriginBlueprintPreviewCompensationTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper TestWorld;
    if (!TestTrue(TEXT("Create isolated preview simulation world"),
        TestWorld.CreateTestWorld(EWorldType::Game))) return false;
    UWorld* World = TestWorld.GetTestWorld();
    if (!TestNotNull(TEXT("Preview fixture world"), World)) return false;
    FActorSpawnParameters Params;
    Params.ObjectFlags |= RF_Transient;
    AActor* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
    if (!TestNotNull(TEXT("Preview fixture actor"), Actor)) return false;
    USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("TestRoot"), RF_Transient);
    UOriginPivotComponent* Pivot = NewObject<UOriginPivotComponent>(Actor, TEXT("TestPivot"), RF_Transient);
    USceneComponent* Child = NewObject<USceneComponent>(Actor, TEXT("TestChild"), RF_Transient);
    USceneComponent* Grandchild = NewObject<USceneComponent>(Actor, TEXT("TestGrandchild"), RF_Transient);
    Actor->AddInstanceComponent(Root);
    Actor->AddInstanceComponent(Pivot);
    Actor->AddInstanceComponent(Child);
    Actor->AddInstanceComponent(Grandchild);
    Actor->SetRootComponent(Root);
    Pivot->SetupAttachment(Root);
    Child->SetupAttachment(Pivot);
    Grandchild->SetupAttachment(Child);
    Root->RegisterComponent();
    Pivot->RegisterComponent();
    Child->RegisterComponent();
    Grandchild->RegisterComponent();
    Pivot->SetRelativeLocation(FVector(20, 0, 0));
    Child->SetRelativeLocation(FVector(100, 30, 0));
    Grandchild->SetRelativeLocation(FVector(10, 5, 0));
    const FTransform BeforePivot = Pivot->GetRelativeTransform();
    const FTransform BeforeChild = Child->GetRelativeTransform();
    const FTransform OriginalChildWorld = Child->GetComponentTransform();
    const FTransform OriginalGrandchildWorld = Grandchild->GetComponentTransform();
    for (int32 Index = 0; Index < 40; ++Index)
    {
        // Simulates the Blueprint editor moving the real preview component
        // rather than calling Origin's legacy FEdMode InputDelta.
        Pivot->SetRelativeLocationAndRotation(FVector(20 + Index * 2.0, Index * .5, 0),
            FRotator(0, Index * .5, 0));
        if (!TestTrue(TEXT("Compensate native preview component movement"),
                OriginBlueprintPivot::ReconcilePreviewChild(Pivot, Child, BeforePivot, BeforeChild))) return false;
        if (!TestTrue(TEXT("Child world position preserved after native movement"),
                Child->GetComponentTransform().Equals(OriginalChildWorld, 1.e-2))) return false;
        if (!TestTrue(TEXT("Nested grandchild world transform preserved"),
                Grandchild->GetComponentTransform().Equals(OriginalGrandchildWorld, 1.e-2))) return false;
    }
    World->DestroyActor(Actor);
    return true;
}

ORIGIN_RECONSTRUCTION_TEST(FOriginBlueprintTemplatePersistenceTest, "Origin.Editor.Blueprint.TemplateEdit.CompilePreservesChildren")
bool FOriginBlueprintTemplatePersistenceTest::RunTest(const FString& Parameters)
{
    FTransientBlueprintFixture Fixture;
    if (!Fixture.Build(*this, 1)) return false;
    UBlueprint* BP = Fixture.Blueprint.Get();
    USimpleConstructionScript* SCS = BP ? BP->SimpleConstructionScript : nullptr;
    if (!TestNotNull(TEXT("Compiled Blueprint SCS"), SCS)) return false;
    USCS_Node* PivotNode = SCS->FindSCSNode(TEXT("Branch0"));
    USCS_Node* ChildNode = SCS->FindSCSNode(TEXT("Branch1"));
    UOriginPivotComponent* Pivot = PivotNode ? Cast<UOriginPivotComponent>(PivotNode->ComponentTemplate.Get()) : nullptr;
    USceneComponent* Child = ChildNode ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
    if (!TestNotNull(TEXT("Authored Origin template"), Pivot) ||
        !TestNotNull(TEXT("Authored direct child template"), Child)) return false;
    const FTransform BeforePivot = Pivot->GetRelativeTransform();
    const FTransform BeforeChild = Child->GetRelativeTransform();
    const FTransform ParentFrame = BeforeChild * BeforePivot;
    FTransform NewPivot = BeforePivot;
    NewPivot.SetLocation(BeforePivot.GetLocation() + FVector(55, -10, 0));
    NewPivot.SetRotation(FRotator(0, 22, 0).Quaternion() * BeforePivot.GetRotation());
    {
        // Directly exercise the real template authoring session, not just
        // isolated FTransform arithmetic. Undo buffer isn't required here.
        FOriginBlueprintPivotEditSession Session(Pivot, false);
        FText Reason;
        if (!TestTrue(TEXT("Apply authored Blueprint pivot edit"), Session.Apply(NewPivot, Reason)))
        {
            AddError(Reason.ToString());
            return false;
        }
        Session.Finish();
    }
    TestTrue(TEXT("Authored direct child preserved before compilation"),
        (Child->GetRelativeTransform() * Pivot->GetRelativeTransform()).Equals(ParentFrame, 1.e-3));
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    USCS_Node* CompiledPivotNode = BP->SimpleConstructionScript->FindSCSNode(TEXT("Branch0"));
    USCS_Node* CompiledChildNode = BP->SimpleConstructionScript->FindSCSNode(TEXT("Branch1"));
    UOriginPivotComponent* CompiledPivot = CompiledPivotNode
        ? Cast<UOriginPivotComponent>(CompiledPivotNode->ComponentTemplate.Get()) : nullptr;
    USceneComponent* CompiledChild = CompiledChildNode
        ? Cast<USceneComponent>(CompiledChildNode->ComponentTemplate.Get()) : nullptr;
    if (!TestNotNull(TEXT("Compiled authored pivot"), CompiledPivot) ||
        !TestNotNull(TEXT("Compiled authored child"), CompiledChild)) return false;
    TestTrue(TEXT("Compiled Blueprint keeps authored pivot offset"),
        CompiledPivot->GetRelativeTransform().Equals(NewPivot, 1.e-3));
    TestTrue(TEXT("Compile preserves direct child parent-frame transform"),
        (CompiledChild->GetRelativeTransform() * CompiledPivot->GetRelativeTransform()).Equals(ParentFrame, 1.e-3));
    return true;
}

#undef ORIGIN_RECONSTRUCTION_TEST
#endif
