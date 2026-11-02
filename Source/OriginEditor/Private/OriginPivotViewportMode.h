// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"
#include "OriginPivotEditSession.h"
#include "OriginBlueprintPivotEdit.h"

class UOriginPivotComponent;
class UBlueprint;
class USCS_Node;

/** Explicit opt-in Level Editor mode. Native W/E widget input, no actor replacement. */
class FOriginPivotViewportMode final : public FEdMode
{
public:
    static const FEditorModeID ModeID;
    virtual ~FOriginPivotViewportMode() override;
    static bool IsEditing(const UOriginPivotComponent* Pivot);
    static bool Toggle(UOriginPivotComponent* Pivot);
    static void Shutdown();
    /** Blueprint ITF gizmos need not populate the legacy widget axis. */
    static bool CanStartTracking(bool bBlueprint, bool bHasLegacyWidgetAxis)
    {
        return bBlueprint || bHasLegacyWidgetAxis;
    }
    // Read-only world-space offsets from the position/rotation when viewport editing began.
    // They are already applied, not pending numeric edits.
    static bool GetLiveEditDeltas(const UOriginPivotComponent* Pivot, FVector& OutTranslationCm, FRotator& OutRotationDegrees);
    static void CalculateEditDeltas(const FTransform& InitialWorld, const FTransform& CurrentWorld,
        FVector& OutTranslationCm, FRotator& OutRotationDegrees);

    void SetTarget(UOriginPivotComponent* Pivot);
    bool IsBlueprintEditing() const { return BlueprintAsset.IsValid(); }
    virtual void Enter() override;
    virtual void Exit() override;
    virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;
    virtual bool UsesTransformWidget() const override { return true; }
    virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override;
    virtual bool StartTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;
    virtual bool InputDelta(FEditorViewportClient* ViewportClient, FViewport* Viewport,
        FVector& Drag, FRotator& Rotation, FVector& Scale) override;
    virtual bool EndTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;
    virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
        FKey Key, EInputEvent Event) override;
    virtual FVector GetWidgetLocation() const;

private:
    bool IsValidTarget() const;
    void RefreshWidget() const;
    /** Repair preview/SCS children if the Blueprint viewport processed a native
     * component transform independently of the legacy FEdMode InputDelta path. */
    void ReconcileBlueprintDrag();
    static void FinishEditing();
    FTransform InitialPivotWorld = FTransform::Identity;
    FVector LastBlueprintWidgetLocation = FVector::ZeroVector;
    UE::Widget::EWidgetMode PreviousWidgetMode = UE::Widget::WM_Translate;
    bool bPreviousWidgetVisible = true;
    bool bPreviousDefaultModeActive = false;
    bool bExitRequested = false;
    TWeakObjectPtr<UOriginPivotComponent> Target;
    TUniquePtr<FOriginPivotEditSession> Session;
    TUniquePtr<FOriginBlueprintPivotEditSession> BlueprintSession;
    TWeakObjectPtr<UBlueprint> BlueprintAsset;
    TWeakObjectPtr<USCS_Node> BlueprintNode;
    TWeakObjectPtr<USceneComponent> BlueprintPreviewPivot;
    bool bTracking = false;
    bool bFallbackTracking = false;
};
