// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** Editor-only, opt-in Origin performance diagnostics. No runtime module dependency. */
namespace OriginPerformance
{
    enum class EScope : uint8
    {
        OutlinerTick,
        OutlinerRebuild,
        OutlinerMouseFeedback,
        PivotViewportTick,
        BlueprintPivotPlan,
        BlueprintPivotApply,
        Count
    };

    struct FSummary
    {
        int32 Count = 0;
        double MeanMs = 0;
        double MedianMs = 0;
        double P95Ms = 0;
        double MaxMs = 0;
        double MinMs = 0;
    };

    /** Pure, deterministic math used by the native automation suite. */
    FSummary Summarize(const TArray<double>& Milliseconds);

    /** Tiny opt-in measurement. Has no allocation when a capture is not running. */
    class FScopeTimer final
    {
    public:
        explicit FScopeTimer(EScope InScope);
        ~FScopeTimer();
    private:
        EScope Scope;
        double Started = 0.0;
        bool bEnabled = false;
    };

    void Initialize();
    void Shutdown();
    void Start(const FString& Label);
    void StartAuto();
    void Stop();
    void Compare();
    bool IsCapturing();
}

#define ORIGIN_PERF_SCOPE(Name) OriginPerformance::FScopeTimer OriginPerfScope_##Name(OriginPerformance::EScope::Name)
