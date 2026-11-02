// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Native Unreal Automation Framework only. Creates no assets, modifies no user level.
#include "OriginPerformance.h"
#include "OriginPivotComponent.h"
#include "OriginBlueprintPivotEdit.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "HAL/PlatformTime.h"

namespace
{
    struct FMovementFixture
    {
        FTestWorldWrapper TestWorld;
        UWorld* World = nullptr;
        bool Initialize(FAutomationTestBase& Test)
        {
            if (!Test.TestTrue(TEXT("Create isolated non-editor test world"), TestWorld.CreateTestWorld(EWorldType::Game))) return false;
            World = TestWorld.GetTestWorld();
            return Test.TestNotNull(TEXT("Fixture world"), World);
        }
        AActor* Spawn(bool bUseOrigin)
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transient;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
            if (!Actor) return nullptr;
            auto Add = [Actor](USceneComponent* Parent, int32 Index, bool bPivot) -> USceneComponent*
            {
                USceneComponent* Component = bPivot
                    ? static_cast<USceneComponent*>(NewObject<UOriginPivotComponent>(Actor, NAME_None, RF_Transient))
                    : NewObject<USceneComponent>(Actor, NAME_None, RF_Transient);
                if (!Component) return nullptr;
                Component->SetMobility(EComponentMobility::Movable);
                Actor->AddInstanceComponent(Component);
                if (Parent) Component->SetupAttachment(Parent);
                else Actor->SetRootComponent(Component);
                Component->RegisterComponent();
                Component->SetRelativeLocation(FVector(Index * 3.0, Index % 7, 0.0));
                return Component;
            };
            USceneComponent* Root = Add(nullptr, 0, false);
            if (!Root) return nullptr;
            USceneComponent* Branch = Root;
            for (int32 Index = 0; Index < 3; ++Index)
            {
                Branch = Add(Branch, Index + 1, bUseOrigin);
                if (!Branch) return nullptr;
            }
            // Same topology and scene component count with and without Origin.
            for (int32 Index = 0; Index < 48; ++Index)
                if (!Add(Branch, Index + 10, false)) return nullptr;
            return Actor;
        }
    };
    double MeasureActor(AActor* Actor, int32 Iterations)
    {
        const FVector Start = Actor->GetActorLocation();
        for (int32 Index = 0; Index < 32; ++Index)
            Actor->SetActorLocation(Start + FVector(Index % 3, Index % 5, 0));
        const double Before = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Iterations; ++Index)
            Actor->SetActorLocation(Start + FVector(Index % 13, Index % 11, Index % 7));
        const double ElapsedMs = (FPlatformTime::Seconds() - Before) * 1000.;
        Actor->SetActorLocation(Start);
        return ElapsedMs / Iterations;
    }
}

#define ORIGIN_PERFORMANCE_TEST(Class, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_PERFORMANCE_TEST(FOriginPerfPercentileTest, "Origin.Performance.Analytics.Percentiles")
bool FOriginPerfPercentileTest::RunTest(const FString& Parameters)
{
    TArray<double> Series = {10., 5., 2., 3., 11., 6., 4., 7., 1., 8., 9.};
    const OriginPerformance::FSummary Stats = OriginPerformance::Summarize(Series);
    TestEqual(TEXT("Frame count"), Stats.Count, 11);
    TestTrue(TEXT("Median of odd frame count"), FMath::IsNearlyEqual(Stats.MedianMs, 6.));
    TestTrue(TEXT("P95 nearest rank"), FMath::IsNearlyEqual(Stats.P95Ms, 11.));
    TestTrue(TEXT("Mean"), FMath::IsNearlyEqual(Stats.MeanMs, 6.));
    TestTrue(TEXT("Maximum"), FMath::IsNearlyEqual(Stats.MaxMs, 11.));
    const auto Empty = OriginPerformance::Summarize(TArray<double>{});
    TestEqual(TEXT("Empty captures return zero samples"), Empty.Count, 0);
    const OriginPerformance::FSummary Even = OriginPerformance::Summarize(TArray<double>{2., 8., 4., 6.});
    TestTrue(TEXT("Even-sized median"), FMath::IsNearlyEqual(Even.MedianMs, 5.));
    return true;
}

ORIGIN_PERFORMANCE_TEST(FOriginPerfMovementBenchmark, "Origin.Performance.ActorMovement.NativeVsOrigin")
bool FOriginPerfMovementBenchmark::RunTest(const FString& Parameters)
{
    FMovementFixture Fixture;
    if (!Fixture.Initialize(*this)) return false;
    AActor* Native = Fixture.Spawn(false);
    AActor* Origin = Fixture.Spawn(true);
    if (!TestNotNull(TEXT("Native scene-component Actor"), Native) ||
        !TestNotNull(TEXT("Equivalent Origin pivot Actor"), Origin)) return false;
    constexpr int32 Iterations = 400;
    // Multiple passes mitigate a single warm cache/JIT/OS scheduling anomaly.
    const double NativeMs = FMath::Min(MeasureActor(Native, Iterations), MeasureActor(Native, Iterations));
    const double OriginMs = FMath::Min(MeasureActor(Origin, Iterations), MeasureActor(Origin, Iterations));
    TestTrue(TEXT("Native movement sample positive"), NativeMs > 0 && FMath::IsFinite(NativeMs));
    TestTrue(TEXT("Origin movement sample positive"), OriginMs > 0 && FMath::IsFinite(OriginMs));
    const double Ratio = NativeMs > 0 ? OriginMs / NativeMs : 0.;
    AddInfo(FString::Printf(TEXT("Origin synthetic Actor movement: native=%.5f ms/update, Origin=%.5f ms/update, ratio=%.2fx, 52 components each, 400 moves/pass. Informational measurement only. This is NOT viewport FPS, rendering, Slate, or Construction Script cost."), NativeMs, OriginMs, Ratio));
    return true;
}

ORIGIN_PERFORMANCE_TEST(FOriginPerfOrdinaryMovementTest, "Origin.Performance.ActorMovement.OrdinaryMovementPreservesRelativeTransforms")
bool FOriginPerfOrdinaryMovementTest::RunTest(const FString& Parameters)
{
    FMovementFixture Fixture;
    if (!Fixture.Initialize(*this)) return false;
    AActor* OriginActor = Fixture.Spawn(true);
    if (!TestNotNull(TEXT("Origin-equipped Actor"), OriginActor)) return false;
    TArray<UOriginPivotComponent*> Pivots;
    OriginActor->GetComponents<UOriginPivotComponent>(Pivots);
    TestEqual(TEXT("Three intermediate Origin pivot components"), Pivots.Num(), 3);
    TArray<FTransform> Before;
    for (UOriginPivotComponent* Pivot : Pivots) Before.Add(Pivot->GetRelativeTransform());
    for (int32 Index = 0; Index < 240; ++Index)
        OriginActor->SetActorLocation(FVector(Index % 9, Index % 7, Index % 5));
    for (int32 Index = 0; Index < Pivots.Num(); ++Index)
        TestTrue(TEXT("Regular Actor movement must not author/change Origin pivot relative offsets"),
            Pivots[Index]->GetRelativeTransform().Equals(Before[Index], 1.e-4));
    return true;
}

ORIGIN_PERFORMANCE_TEST(FOriginPerfRepeatedCompensationTest, "Origin.Editor.Blueprint.Math.ManySmallDeltasStayPreserved")
bool FOriginPerfRepeatedCompensationTest::RunTest(const FString& Parameters)
{
    const FTransform InitialPivot(FRotator(0, 20, 0), FVector(42, -15, 3));
    const FTransform InitialChild(FRotator(0, 10, 0), FVector(120, 5, 0));
    const FTransform Original = InitialChild * InitialPivot;
    FTransform LatestPivot = InitialPivot;
    for (int32 Index = 0; Index < 300; ++Index)
    {
        LatestPivot.AddToTranslation(FVector(0.125, -0.08, 0.03));
        FTransform Compensated;
        if (!TestTrue(TEXT("Each cumulative Blueprint pivot delta compensates"),
            OriginBlueprintPivot::CompensateChild(InitialPivot, LatestPivot, InitialChild, Compensated))) return false;
        if (!TestTrue(TEXT("Child remains fixed through repeated small deltas"),
            (Compensated * LatestPivot).Equals(Original, 1.e-3))) return false;
    }
    return true;
}
#undef ORIGIN_PERFORMANCE_TEST
#endif
