// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginPerformance.h"

#include "OriginPivotComponent.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Selection.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_LOG_CATEGORY_STATIC(LogOriginPerf, Log, All);

namespace OriginPerformance
{
    namespace
    {
        constexpr int32 ScopeCount = static_cast<int32>(EScope::Count);
        constexpr int32 MaxFrameSamples = 10000;
        const TCHAR* const ScopeNames[ScopeCount] = {
            TEXT("OutlinerTick"), TEXT("OutlinerRebuild"), TEXT("OutlinerMouseFeedback"),
            TEXT("PivotViewportTick"), TEXT("BlueprintPivotPlan"), TEXT("BlueprintPivotApply")
        };
        struct FFrame
        {
            bool bMoving = false;
            double FrameMs = 0.0;
            double ScopeMs[ScopeCount] = {};
        };
        struct FReport
        {
            FString Label;
            FString ActorName;
            TWeakObjectPtr<AActor> SelectedActor;
            bool bAutoClassify = false;
            int32 SceneComponents = 0;
            int32 OriginPivots = 0;
            TArray<FFrame> Frames;
            int64 Calls[ScopeCount] = {};
            double TotalsMs[ScopeCount] = {};
            FSummary FrameSummary;
            FString CSVPath;
        };
        bool bCapturing = false;
        FReport Current;
        TArray<FReport> Completed;
        double FrameScopes[ScopeCount] = {};
        FTSTicker::FDelegateHandle TickHandle;
        FDelegateHandle MovingHandle;
        uint64 LastMovementFrame = 0;
        bool bReceivedMovementEvent = false;

        FString SafeLabel(const FString& Label)
        {
            FString Result = Label.IsEmpty() ? TEXT("Capture") : Label;
            for (int32 Index = 0; Index < Result.Len(); ++Index)
                if (!FChar::IsAlnum(Result[Index]) && Result[Index] != TCHAR('_') && Result[Index] != TCHAR('-'))
                    Result[Index] = TCHAR('_');
            return Result.Left(48);
        }
        void WriteActorDetails(FReport& Report)
        {
            Report.ActorName = TEXT("<none selected>");
            if (!GEditor) return;
            USelection* Selection = GEditor->GetSelectedActors();
            AActor* Actor = Selection ? Selection->GetTop<AActor>() : nullptr;
            if (!IsValid(Actor)) return;
            Report.ActorName = Actor->GetName();
            Report.SelectedActor = Actor;
            TArray<USceneComponent*> Components;
            Actor->GetComponents<USceneComponent>(Components);
            Report.SceneComponents = Components.Num();
            for (const USceneComponent* Component : Components)
                if (Component && Component->IsA<UOriginPivotComponent>()) ++Report.OriginPivots;
        }
        bool OnTick(float DeltaSeconds)
        {
            if (!bCapturing) return true;
            if (Current.Frames.Num() >= MaxFrameSamples)
            {
                // Do not allocate unbounded arrays if someone forgets to stop a capture.
                Stop();
                return true;
            }
            FFrame& Frame = Current.Frames.AddDefaulted_GetRef();
            Frame.FrameMs = FMath::Max(0.0, static_cast<double>(DeltaSeconds) * 1000.0);
            Frame.bMoving = Current.bAutoClassify && bReceivedMovementEvent && GFrameCounter <= LastMovementFrame + 2;
            for (int32 Index = 0; Index < ScopeCount; ++Index)
            {
                Frame.ScopeMs[Index] = FrameScopes[Index];
                FrameScopes[Index] = 0;
            }
            return true;
        }
        FSummary BuildFrameSummary(const FReport& Report)
        {
            TArray<double> Values;
            Values.Reserve(Report.Frames.Num());
            for (const FFrame& Frame : Report.Frames) Values.Add(Frame.FrameMs);
            return Summarize(Values);
        }
        FString SummaryString(const FReport& Report)
        {
            const FSummary& S = Report.FrameSummary;
            return FString::Printf(TEXT("%s: %d frames | mean %.2f ms (%.1f FPS) | median %.2f ms | P95 %.2f ms | worst %.2f ms | selected=%s | scene components=%d | Origin pivots=%d"),
                *Report.Label, S.Count, S.MeanMs, S.MeanMs > 0 ? 1000.0/S.MeanMs : 0.,
                S.MedianMs, S.P95Ms, S.MaxMs, *Report.ActorName, Report.SceneComponents, Report.OriginPivots);
        }
        void ExportReport(FReport& Report)
        {
            const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Origin"), TEXT("Diagnostics"));
            IFileManager::Get().MakeDirectory(*Folder, true);
            Report.CSVPath = FPaths::Combine(Folder, FString::Printf(TEXT("OriginPerf_%s_%s.csv"),
                *SafeLabel(Report.Label), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
            FString CSV = TEXT("frame,phase,frame_ms");
            CSV.Reserve(Report.Frames.Num() * 110 + 128);
            for (const TCHAR* ScopeName : ScopeNames) CSV += FString::Printf(TEXT(",%s_ms"), ScopeName);
            CSV += TEXT("\n");
            for (int32 FrameIndex = 0; FrameIndex < Report.Frames.Num(); ++FrameIndex)
            {
                const FFrame& Frame = Report.Frames[FrameIndex];
                CSV += FString::Printf(TEXT("%d,%s,%.5f"), FrameIndex, Frame.bMoving ? TEXT("Moving") : TEXT("Idle"), Frame.FrameMs);
                for (double ScopeMs : Frame.ScopeMs) CSV += FString::Printf(TEXT(",%.5f"), ScopeMs);
                CSV += TEXT("\n");
            }
            if (!FFileHelper::SaveStringToFile(CSV, *Report.CSVPath))
                UE_LOG(LogOriginPerf, Error, TEXT("Could not write performance CSV %s"), *Report.CSVPath);
            const FString TextPath = FPaths::ChangeExtension(Report.CSVPath, TEXT("txt"));
            FString Summary = SummaryString(Report) + TEXT("\n");
            if (Report.bAutoClassify)
            {
                TArray<double> Idle, Moving;
                for (const FFrame& Frame : Report.Frames)
                    (Frame.bMoving ? Moving : Idle).Add(Frame.FrameMs);
                const FSummary IdleStats = Summarize(Idle);
                const FSummary MovingStats = Summarize(Moving);
                Summary += FString::Printf(TEXT("Auto-phases: idle %d frames, %.2f ms mean, %.2f P95 | moving %d frames, %.2f ms mean, %.2f P95 | movement slowdown %.2fx\n"),
                    IdleStats.Count, IdleStats.MeanMs, IdleStats.P95Ms,
                    MovingStats.Count, MovingStats.MeanMs, MovingStats.P95Ms,
                    IdleStats.MeanMs > 0 ? MovingStats.MeanMs/IdleStats.MeanMs : 0.0);
                if (MovingStats.Count == 0)
                    Summary += TEXT("No selected-Actor movement events were observed. Did you select the Actor before capture, and drag the Actor in the Level Editor?\n");
                for (int32 ScopeIndex = 0; ScopeIndex < ScopeCount; ++ScopeIndex)
                {
                    double IdleTotal = 0.0;
                    double MovingTotal = 0.0;
                    for (const FFrame& Frame : Report.Frames)
                    {
                        if (Frame.bMoving) MovingTotal += Frame.ScopeMs[ScopeIndex];
                        else IdleTotal += Frame.ScopeMs[ScopeIndex];
                    }
                    Summary += FString::Printf(TEXT("Origin %s CPU ms/frame: idle %.5f | moving %.5f\n"),
                        ScopeNames[ScopeIndex], IdleStats.Count ? IdleTotal/IdleStats.Count : 0.0,
                        MovingStats.Count ? MovingTotal/MovingStats.Count : 0.0);
                }
            }
            for (int32 Index = 0; Index < ScopeCount; ++Index)
                Summary += FString::Printf(TEXT("%s: %lld calls, %.4f ms total, %.5f ms/frame average\n"),
                    ScopeNames[Index], static_cast<long long>(Report.Calls[Index]), Report.TotalsMs[Index],
                    Report.Frames.Num() ? Report.TotalsMs[Index] / Report.Frames.Num() : 0.0);
            Summary += TEXT("\nImportant: Samples are editor ticker intervals, not stat unit Game/Draw/GPU times. CPU scopes cover ONLY Origin functions. If frame time is high and Origin scopes are low, use Unreal Insights to find the work elsewhere. Benchmark an equivalent Actor without Origin to separate component hierarchy cost.\n");
            FFileHelper::SaveStringToFile(Summary, *TextPath);
            UE_LOG(LogOriginPerf, Display, TEXT("%s"), *Summary);
            UE_LOG(LogOriginPerf, Display, TEXT("Capture CSV: %s"), *Report.CSVPath);
            UE_LOG(LogOriginPerf, Display, TEXT("Summary TXT: %s"), *TextPath);
        }
    }

    FSummary Summarize(const TArray<double>& Milliseconds)
    {
        FSummary Result;
        if (Milliseconds.IsEmpty()) return Result;
        TArray<double> Values = Milliseconds;
        Values.Sort();
        Result.Count = Values.Num();
        Result.MinMs = Values[0];
        Result.MaxMs = Values.Last();
        Result.MedianMs = Values[(Values.Num() - 1)/2];
        if (Values.Num()%2 == 0) Result.MedianMs = (Values[Values.Num()/2 - 1] + Values[Values.Num()/2])*0.5;
        Result.P95Ms = Values[FMath::Clamp(FMath::CeilToInt(Values.Num()*0.95)-1, 0, Values.Num()-1)];
        double Sum = 0;
        for (double V : Values) Sum += V;
        Result.MeanMs = Sum/Values.Num();
        return Result;
    }

    bool IsCapturing() { return bCapturing; }

    FScopeTimer::FScopeTimer(EScope InScope) : Scope(InScope)
    {
        bEnabled = bCapturing;
        if (bEnabled) Started = FPlatformTime::Seconds();
    }
    FScopeTimer::~FScopeTimer()
    {
        if (!bEnabled || !bCapturing) return;
        const int32 Index = static_cast<int32>(Scope);
        const double ElapsedMs = (FPlatformTime::Seconds()-Started)*1000.0;
        FrameScopes[Index] += ElapsedMs;
        Current.Calls[Index]++;
        Current.TotalsMs[Index] += ElapsedMs;
    }
    void Start(const FString& Label)
    {
        if (bCapturing) Stop();
        Current = FReport{};
        Current.Label = Label.IsEmpty() ? TEXT("Capture") : Label;
        WriteActorDetails(Current);
        bReceivedMovementEvent = false;
        for (double& V : FrameScopes) V = 0;
        bCapturing = true;
        UE_LOG(LogOriginPerf, Display, TEXT("Started '%s'. Drag normally, then run Origin.Perf.Stop."), *Current.Label);
    }
    void StartAuto()
    {
        Start(TEXT("ActorDragAuto"));
        Current.bAutoClassify = true;
        if (!Current.SelectedActor.IsValid())
            UE_LOG(LogOriginPerf, Warning, TEXT("Select the problem Actor BEFORE Origin.Perf.Auto; move detection requires that selection."));
        if (GEngine)
            MovingHandle = GEngine->OnActorMoving().AddLambda([](AActor* Actor)
            {
                if (bCapturing && Current.bAutoClassify && Current.SelectedActor.Get() == Actor)
                {
                    LastMovementFrame = GFrameCounter;
                    bReceivedMovementEvent = true;
                }
            });
    }
    void Stop()
    {
        if (!bCapturing) { UE_LOG(LogOriginPerf, Warning, TEXT("No active Origin performance capture.")); return; }
        bCapturing = false;
        if (MovingHandle.IsValid() && GEngine) GEngine->OnActorMoving().Remove(MovingHandle);
        MovingHandle.Reset();
        Current.FrameSummary = BuildFrameSummary(Current);
        ExportReport(Current);
        Completed.Add(MoveTemp(Current));
        if (Completed.Num() > 2) Completed.RemoveAt(0);
        for (double& V : FrameScopes) V = 0;
    }
    void Compare()
    {
        if (Completed.Num() < 2)
        {
            UE_LOG(LogOriginPerf, Warning, TEXT("Record two captures first: Origin.Perf.Start Baseline / Stop; Origin.Perf.Start Drag / Stop."));
            return;
        }
        const FReport& A = Completed[Completed.Num()-2];
        const FReport& B = Completed.Last();
        const double Ratio = A.FrameSummary.MeanMs > 0 ? B.FrameSummary.MeanMs/A.FrameSummary.MeanMs : 0;
        UE_LOG(LogOriginPerf, Display, TEXT("COMPARE: %s -> %s | mean frame %.2f -> %.2f ms | P95 %.2f -> %.2f ms | ratio %.2fx | frame-time increase %.2f ms"),
            *A.Label, *B.Label, A.FrameSummary.MeanMs, B.FrameSummary.MeanMs,
            A.FrameSummary.P95Ms, B.FrameSummary.P95Ms, Ratio,
            B.FrameSummary.MeanMs-A.FrameSummary.MeanMs);
        for (int32 Index = 0; Index < ScopeCount; ++Index)
            UE_LOG(LogOriginPerf, Display, TEXT("COMPARE Origin %s CPU ms/frame: %.5f -> %.5f (call counts %lld -> %lld)"),
                ScopeNames[Index], A.Frames.Num() ? A.TotalsMs[Index]/A.Frames.Num() : 0.,
                B.Frames.Num() ? B.TotalsMs[Index]/B.Frames.Num() : 0.,
                static_cast<long long>(A.Calls[Index]), static_cast<long long>(B.Calls[Index]));
        UE_LOG(LogOriginPerf, Display, TEXT("CSV files: %s | %s"), *A.CSVPath, *B.CSVPath);
    }
    void Initialize()
    {
        if (!TickHandle.IsValid())
            TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OnTick));
    }
    void Shutdown()
    {
        if (bCapturing) Stop();
        if (MovingHandle.IsValid() && GEngine) GEngine->OnActorMoving().Remove(MovingHandle);
        MovingHandle.Reset();
        if (TickHandle.IsValid())
            FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }
}

static FAutoConsoleCommand GOriginPerfStart(
    TEXT("Origin.Perf.Start"), TEXT("Start an opt-in frame/Origin CPU capture. Usage: Origin.Perf.Start Baseline"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
    {
        OriginPerformance::Start(Args.Num() ? Args[0] : TEXT("Capture"));
    }));
static FAutoConsoleCommand GOriginPerfStop(
    TEXT("Origin.Perf.Stop"), TEXT("Stop the capture and write Saved/Origin/Diagnostics CSV and TXT."),
    FConsoleCommandDelegate::CreateStatic(&OriginPerformance::Stop));
static FAutoConsoleCommand GOriginPerfCompare(
    TEXT("Origin.Perf.Compare"), TEXT("Compare the last two performance captures."),
    FConsoleCommandDelegate::CreateStatic(&OriginPerformance::Compare));

static FAutoConsoleCommand GOriginPerfAuto(
    TEXT("Origin.Perf.Auto"), TEXT("Automatically split idle/moving frames for the selected Actor. Select Actor first; then Origin.Perf.Auto, drag, Origin.Perf.Stop."),
    FConsoleCommandDelegate::CreateStatic(&OriginPerformance::StartAuto));
