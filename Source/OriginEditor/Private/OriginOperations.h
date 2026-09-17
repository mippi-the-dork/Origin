#pragma once
#include "CoreMinimal.h"
#include "OriginAnchor.h"

namespace Origin
{
    /** Editor-only snapshot for validating and committing a complete pivot rule. */
    struct FPivotSettings
    {
        EOriginPivotMode Mode = EOriginPivotMode::BoundsCenter;
        TSoftObjectPtr<AActor> Child;
        FVector Custom = FVector::ZeroVector;
        FIntVector BoundsAxes = FIntVector(1,1,1);
        TSoftObjectPtr<AActor> Reference;
        FName ComponentName;
        FName SocketName;
        FVector Offset = FVector::ZeroVector;
        bool bLocalOffset = false;
    };
    FPivotSettings ReadSettings(const AOriginAnchor* Anchor);
    bool ApplySettings(AOriginAnchor* Anchor, const FPivotSettings& Settings);
    bool CanRecalculate(AOriginAnchor* Anchor, FText& Reason);
    bool Recalculate(AOriginAnchor* Anchor);
    bool IsEditorActor(const AActor* Actor);
    bool IsProtected(AActor* Actor);
    void Notify(const FText& Message);
    TArray<AActor*> SelectedActors();
    TArray<AActor*> DirectChildren(AActor* Actor);
    void GatherBranch(AActor* Root, TArray<AActor*>& Out);
    bool CanOperate(AOriginAnchor* Anchor, FText& Reason);
    void CreateAnchor();
    bool SetPivot(AOriginAnchor* Anchor, EOriginPivotMode Mode, AActor* Child, const FVector& Custom);
    void RemoveAnchors(const TArray<AOriginAnchor*>& Anchors, bool bConvert);
    void ConvertSelected();
}
