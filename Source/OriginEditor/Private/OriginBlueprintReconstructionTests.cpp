// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Origin-only Unreal native Automation Framework tests. No project assets are modified.
#include "OriginBlueprintPivotEdit.h"
#include "OriginPerformance.h"

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
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

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

    // A single per-call figure is too sensitive to OS scheduling, CPU clock and
    // Blueprint/object allocator warmup. Each sample is an average of a short
    // batch; the three variants run in a rotating order every round.
    double MeasureReconstructionBatch(AActor* Actor, int32 Iterations)
    {
        const double Started = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Iterations; ++Index)
            Actor->RerunConstructionScripts();
        return (FPlatformTime::Seconds() - Started) * 1000.0 / Iterations;
    }
    constexpr int32 ReconstructionVariants = 3;
    constexpr int32 ReconstructionRounds = 18;
    constexpr int32 ReconstructionCallsPerRound = 24;
    int32 VariantForPosition(int32 Round, int32 Position)
    {
        return (Round + Position) % ReconstructionVariants;
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

    // Build and spawn all variants before timing so Blueprint compilation and
    // first-spawn costs cannot leak into one measured variant.
    FTransientBlueprintFixture Fixtures[ReconstructionVariants];
    AActor* Actors[ReconstructionVariants] = {};
    TArray<double> Samples[ReconstructionVariants];
    for (int32 Count = 0; Count < ReconstructionVariants; ++Count)
    {
        if (!Fixtures[Count].Build(*this, Count)) return false;
        UClass* Generated = Fixtures[Count].Blueprint->GeneratedClass.Get();
        if (!TestNotNull(TEXT("Generated Blueprint class"), Generated)) return false;
        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transient;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Actors[Count] = World->SpawnActor<AActor>(Generated, FVector(Count * 1000., 0., 0.),
            FRotator::ZeroRotator, Params);
        if (!TestNotNull(TEXT("Spawn generated Blueprint Actor"), Actors[Count])) return false;
        TArray<UOriginPivotComponent*> Pivots;
        Actors[Count]->GetComponents<UOriginPivotComponent>(Pivots);
        if (!TestEqual(TEXT("Correct number of pivots in generated actor"), Pivots.Num(), Count)) return false;
        TArray<USceneComponent*> Scenes;
        Actors[Count]->GetComponents<USceneComponent>(Scenes);
        if (!TestEqual(TEXT("Eight equivalent scene component nodes"), Scenes.Num(), 8)) return false;
        for (int32 Warmup = 0; Warmup < 10; ++Warmup)
            Actors[Count]->RerunConstructionScripts();
        Samples[Count].Reserve(ReconstructionRounds);
    }

    FString CSV = TEXT("round,position,origin_pivots,reruns,ms_per_rerun\n");
    for (int32 Round = 0; Round < ReconstructionRounds; ++Round)
    {
        for (int32 Position = 0; Position < ReconstructionVariants; ++Position)
        {
            const int32 Variant = VariantForPosition(Round, Position);
            const double Ms = MeasureReconstructionBatch(Actors[Variant], ReconstructionCallsPerRound);
            if (!TestTrue(TEXT("Finite positive batched reconstruction duration"),
                FMath::IsFinite(Ms) && Ms > 0.0)) return false;
            Samples[Variant].Add(Ms);
            CSV += FString::Printf(TEXT("%d,%d,%d,%d,%.8f\n"),
                Round, Position, Variant, ReconstructionCallsPerRound, Ms);
        }
    }

    OriginPerformance::FSummary Stats[ReconstructionVariants];
    for (int32 Count = 0; Count < ReconstructionVariants; ++Count)
    {
        Stats[Count] = OriginPerformance::Summarize(Samples[Count]);
        if (!TestEqual(TEXT("Every case has all balanced rounds"), Stats[Count].Count, ReconstructionRounds)) return false;
        TArray<UOriginPivotComponent*> Pivots;
        Actors[Count]->GetComponents<UOriginPivotComponent>(Pivots);
        if (!TestEqual(TEXT("Repeated reconstruction retains Origin component count"), Pivots.Num(), Count)) return false;
        TArray<USceneComponent*> Scenes;
        Actors[Count]->GetComponents<USceneComponent>(Scenes);
        if (!TestEqual(TEXT("Repeated reconstruction retains scene hierarchy size"), Scenes.Num(), 8)) return false;
    }

    AddInfo(FString::Printf(TEXT("Origin balanced Blueprint reconstruction: %d warmup, %d rounds x %d reruns = %d measured reruns per variant. NATIVE mean/median/P95=%.4f/%.4f/%.4f ms; ONE Pivot=%.4f/%.4f/%.4f ms (median %.2fx); TWO Pivots=%.4f/%.4f/%.4f ms (median %.2fx). Values are batch-averaged CPU-only synthetic measurements; NOT real viewport FPS or user-authored Construction Script graph."),
        10, ReconstructionRounds, ReconstructionCallsPerRound,
        ReconstructionRounds * ReconstructionCallsPerRound,
        Stats[0].MeanMs, Stats[0].MedianMs, Stats[0].P95Ms,
        Stats[1].MeanMs, Stats[1].MedianMs, Stats[1].P95Ms,
        Stats[0].MedianMs > 0.0 ? Stats[1].MedianMs / Stats[0].MedianMs : 0.0,
        Stats[2].MeanMs, Stats[2].MedianMs, Stats[2].P95Ms,
        Stats[0].MedianMs > 0.0 ? Stats[2].MedianMs / Stats[0].MedianMs : 0.0));

    const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Origin"), TEXT("Diagnostics"));
    IFileManager::Get().MakeDirectory(*Folder, true);
    const FString Path = FPaths::Combine(Folder,
        FString::Printf(TEXT("OriginReconstruction_Balanced_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
    if (FFileHelper::SaveStringToFile(CSV, *Path))
        AddInfo(FString::Printf(TEXT("Origin reconstruction batch CSV: %s"), *Path));
    else
        AddWarning(FString::Printf(TEXT("Could not save optional reconstruction CSV: %s"), *Path));

    for (AActor* Actor : Actors)
        World->DestroyActor(Actor);
    return true; // Informational timing; never assert on an arbitrary speed ratio.
}

ORIGIN_RECONSTRUCTION_TEST(FOriginBlueprintBalancedOrderTest, "Origin.Performance.ConstructionScript.BalancedOrderContract")
bool FOriginBlueprintBalancedOrderTest::RunTest(const FString& Parameters)
{
    int32 Positions[ReconstructionVariants][ReconstructionVariants] = {};
    for (int32 Round = 0; Round < ReconstructionRounds; ++Round)
    {
        bool bSeen[ReconstructionVariants] = {};
        for (int32 Position = 0; Position < ReconstructionVariants; ++Position)
        {
            const int32 Variant = VariantForPosition(Round, Position);
            if (!TestTrue(TEXT("Variant index in range"), Variant >= 0 && Variant < ReconstructionVariants)) return false;
            if (!TestFalse(TEXT("No variant occurs twice within a round"), bSeen[Variant])) return false;
            bSeen[Variant] = true;
            ++Positions[Variant][Position];
        }
    }
    for (int32 Variant = 0; Variant < ReconstructionVariants; ++Variant)
        for (int32 Position = 0; Position < ReconstructionVariants; ++Position)
            TestEqual(TEXT("Every variant occupies every position equally often"),
                Positions[Variant][Position], ReconstructionRounds / ReconstructionVariants);
    return true;
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
