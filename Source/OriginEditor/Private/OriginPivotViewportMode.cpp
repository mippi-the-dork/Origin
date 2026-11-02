// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginPivotViewportMode.h"

#include "OriginPerformance.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "OriginPivotComponent.h"
#include "OriginBlueprintPivotEdit.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Interfaces/IPluginManager.h"
#include "OriginOperations.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Selection.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "InputCoreTypes.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Framework/Application/SlateApplication.h"

DEFINE_LOG_CATEGORY_STATIC(LogOriginPivotViewport, Log, All);

const FEditorModeID FOriginPivotViewportMode::ModeID = TEXT("EM_OriginPivotViewportEdit");
FOriginPivotViewportMode::~FOriginPivotViewportMode() = default;

namespace
{
    // Asset editors own independent mode managers; never activate a Blueprint
    // gizmo mode on the global Level Editor manager.
    TWeakObjectPtr<UBlueprint> ActiveBlueprint;

    FEditorModeTools* FindBlueprintTools(UBlueprint* Blueprint)
    {
        if (!GEditor || !IsValid(Blueprint)) return nullptr;
        UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        IAssetEditorInstance* Instance = Editors ? Editors->FindEditorForAsset(Blueprint, false) : nullptr;
        if (!Instance) return nullptr;
        // Only normal Blueprint editors use this toolkit; no unchecked casts
        // to unrelated asset editor implementations.
        const FName EditorName = Instance->GetEditorName();
        if (EditorName != FName(TEXT("BlueprintEditor")) && EditorName != FName(TEXT("Kismet"))) return nullptr;
        return &static_cast<FAssetEditorToolkit*>(Instance)->GetEditorModeManager();
    }

    FEditorModeTools* GetToolsFor(UOriginPivotComponent* Pivot)
    {
        UBlueprint* Blueprint = nullptr;
        USCS_Node* Node = nullptr;
        if (OriginBlueprintPivot::Resolve(Pivot, Blueprint, Node)) return FindBlueprintTools(Blueprint);
        return GEditor ? &GLevelEditorModeTools() : nullptr;
    }

    FOriginPivotViewportMode* GetActiveOriginMode()
    {
        if (UBlueprint* BP = ActiveBlueprint.Get())
        {
            if (FEditorModeTools* Tools = FindBlueprintTools(BP))
            {
                if (FEdMode* Mode = Tools->GetActiveMode(FOriginPivotViewportMode::ModeID))
                    return static_cast<FOriginPivotViewportMode*>(Mode);
            }
        }
        if (!GEditor) return nullptr;
        return static_cast<FOriginPivotViewportMode*>(GLevelEditorModeTools().GetActiveMode(FOriginPivotViewportMode::ModeID));
    }
}

void FOriginPivotViewportMode::CalculateEditDeltas(const FTransform& InitialWorld, const FTransform& CurrentWorld,
    FVector& OutTranslationCm, FRotator& OutRotationDegrees)
{
    OutTranslationCm = CurrentWorld.GetLocation() - InitialWorld.GetLocation();
    const FQuat WorldRotationDelta = CurrentWorld.GetRotation() * InitialWorld.GetRotation().Inverse();
    OutRotationDegrees = WorldRotationDelta.Rotator().GetNormalized();
}

bool FOriginPivotViewportMode::GetLiveEditDeltas(const UOriginPivotComponent* Pivot,
    FVector& OutTranslationCm, FRotator& OutRotationDegrees)
{
    const FOriginPivotViewportMode* Mode = GetActiveOriginMode();
    if (!Pivot || !Mode || Mode->Target.Get() != Pivot || !Mode->IsValidTarget()) return false;
    const USceneComponent* Preview = Mode->IsBlueprintEditing()
        ? OriginBlueprintPivot::FindPreviewComponent(Mode->BlueprintAsset.Get(), Mode->BlueprintNode.Get()) : nullptr;
    // An editor preview is transient and may be rebuilt while manipulating the
    // authored SCS template. Never interpret a stale preview as an edit delta.
    if (Mode->IsBlueprintEditing() && !Preview) return false;
    CalculateEditDeltas(Mode->InitialPivotWorld,
        Preview ? Preview->GetComponentTransform() : Pivot->GetComponentTransform(), OutTranslationCm, OutRotationDegrees);
    return true;
}

bool FOriginPivotViewportMode::IsEditing(const UOriginPivotComponent* Pivot)
{
    const FOriginPivotViewportMode* Mode = GetActiveOriginMode();
    return Mode && Pivot && Mode->Target.Get() == Pivot && Mode->IsValidTarget();
}

bool FOriginPivotViewportMode::Toggle(UOriginPivotComponent* Pivot)
{
    if (!GEditor || !IsValid(Pivot) || GEditor->PlayWorld) return false;
    if (IsEditing(Pivot))
    {
        FinishEditing();
        return false;
    }
    FEditorModeTools* Tools = GetToolsFor(Pivot);
    if (!Tools)
    {
        Origin::Notify(FText::FromString(TEXT("Origin: Could not find the Blueprint editor mode manager. Open the Blueprint Viewport and select the authored Origin Pivot component.")));
        return false;
    }

    UBlueprint* Blueprint = nullptr;
    USCS_Node* Node = nullptr;
    const bool bBlueprint = OriginBlueprintPivot::Resolve(Pivot, Blueprint, Node);
    if (bBlueprint)
    {
        // A preview component is required for a viewport gizmo. A Construction
        // Script may override the authored templates: never silently write
        // preview-only offsets back into the Blueprint source in that case.
        USceneComponent* Preview = OriginBlueprintPivot::FindPreviewComponent(Blueprint, Node);
        if (!Preview)
        {
            Origin::Notify(FText::FromString(TEXT("Origin: The Blueprint editor's active preview component could not be resolved from its generated component property. Open the Blueprint Viewport and compile the Blueprint, then try again. No changes were made.")));
            return false;
        }
        if (!OriginBlueprintPivot::IsUnmodifiedPreviewTransform(
                Pivot->GetRelativeTransform(), Preview->GetRelativeTransform()))
        {
            UE_LOG(LogTemp, Warning, TEXT("Origin: Blueprint pivot preview differs from authored template. Template: %s; Preview: %s"),
                *Pivot->GetRelativeTransform().ToHumanReadableString(),
                *Preview->GetRelativeTransform().ToHumanReadableString());
            Origin::Notify(FText::FromString(TEXT("Origin: The Blueprint's preview pivot has a different relative transform than its authored template, potentially from its Construction Script. Viewport pivot editing was refused; see Output Log for both transforms.")));
            return false;
        }
        for (USCS_Node* ChildNode : Node->GetChildNodes())
        {
            USceneComponent* ChildTemplate = ChildNode ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
            USceneComponent* ChildPreview = ChildNode
                ? OriginBlueprintPivot::FindPreviewComponent(Blueprint, ChildNode) : nullptr;
            if (!ChildTemplate || !ChildPreview)
            {
                Origin::Notify(FText::FromString(TEXT("Origin: An authored child has no matching component on the Blueprint Editor preview Actor. No pivot edit was started.")));
                return false;
            }
            if (ChildPreview->GetAttachParent() != Preview)
            {
                Origin::Notify(FText::FromString(TEXT("Origin: The Blueprint preview has changed the pivot's attachment hierarchy. Cannot safely preserve children; no changes were made.")));
                return false;
            }
            if (!OriginBlueprintPivot::IsUnmodifiedPreviewTransform(
                    ChildTemplate->GetRelativeTransform(), ChildPreview->GetRelativeTransform()))
            {
                UE_LOG(LogTemp, Warning, TEXT("Origin: Child %s differs from authored template. Template: %s; Preview: %s"),
                    *ChildNode->GetVariableName().ToString(),
                    *ChildTemplate->GetRelativeTransform().ToHumanReadableString(),
                    *ChildPreview->GetRelativeTransform().ToHumanReadableString());
                Origin::Notify(FText::FromString(TEXT("Origin: A Blueprint child preview has a different relative transform than its authored template. Editing was refused to protect Construction Script changes; see Output Log.")));
                return false;
            }
        }
        // Dynamically attached direct children have no matching source SCS
        // template to compensate. Refuse this authoring mode for those cases.
        for (USceneComponent* Attached : Preview->GetAttachChildren())
        {
            bool bAuthoredChild = false;
            for (USCS_Node* ChildNode : Node->GetChildNodes())
            {
                if (ChildNode && OriginBlueprintPivot::FindPreviewComponent(Blueprint, ChildNode) == Attached)
                {
                    bAuthoredChild = true;
                    break;
                }
            }
            if (!bAuthoredChild)
            {
                Origin::Notify(FText::FromString(TEXT("Origin: The Blueprint preview has an attached component without a corresponding authored SCS node. No pivot edit was started.")));
                return false;
            }
        }
    }
    else
    {
        const USelection* Selection = GEditor->GetSelectedComponents();
        if (!Selection || !Selection->IsSelected(Pivot) || !Pivot->IsRegistered() || Pivot->IsTemplate() ||
            !Pivot->GetWorld() || Pivot->GetWorld()->WorldType != EWorldType::Editor) return false;
    }
    // At most one Origin edit session across all editor viewports.
    if (GetActiveOriginMode()) FinishEditing();
    const bool bWasDefault = Tools->IsDefaultModeActive();
    const bool bWasVisible = Tools->GetShowWidget();
    const UE::Widget::EWidgetMode OldWidgetMode = Tools->GetWidgetMode();
    if (bBlueprint) ActiveBlueprint = Blueprint;
    Tools->ActivateMode(ModeID, false);
    FOriginPivotViewportMode* Mode = static_cast<FOriginPivotViewportMode*>(Tools->GetActiveMode(ModeID));
    if (!Mode)
    {
        if (bBlueprint) ActiveBlueprint.Reset();
        return false;
    }
    Mode->bPreviousDefaultModeActive = bWasDefault;
    Mode->bPreviousWidgetVisible = bWasVisible;
    Mode->PreviousWidgetMode = OldWidgetMode;
    Mode->SetTarget(Pivot);
    if (OldWidgetMode != UE::Widget::WM_Translate && OldWidgetMode != UE::Widget::WM_Rotate)
        Tools->SetWidgetMode(UE::Widget::WM_Translate);
    Tools->SetShowWidget(true);
    return true;
}

void FOriginPivotViewportMode::FinishEditing()
{
    if (!GEditor) return;
    FOriginPivotViewportMode* Mode = GetActiveOriginMode();
    if (!Mode) return;
    const bool bBlueprint = Mode->IsBlueprintEditing();
    UBlueprint* BP = Mode->BlueprintAsset.Get();
    FEditorModeTools* Tools = bBlueprint ? FindBlueprintTools(BP) : &GLevelEditorModeTools();
    if (!Tools) return;
    const bool bRestoreDefault = Mode->bPreviousDefaultModeActive;
    const bool bShowWidget = Mode->bPreviousWidgetVisible;
    const UE::Widget::EWidgetMode SavedWidgetMode = Mode->PreviousWidgetMode;
    // Finish an existing drag before Exit(), which cancels active sessions.
    if (Mode->Session) Mode->Session->Finish();
    if (Mode->BlueprintSession) Mode->BlueprintSession->Finish();
    Mode->Session.Reset();
    Mode->BlueprintSession.Reset();
    Tools->DeactivateMode(ModeID);
    if (bRestoreDefault && !Tools->IsDefaultModeActive()) Tools->ActivateDefaultMode();
    Tools->SetWidgetMode(SavedWidgetMode);
    Tools->SetShowWidget(bShowWidget);
    Tools->SetCurrentWidgetAxis(EAxisList::None);
    if (!bBlueprint && GUnrealEd) GUnrealEd->UpdatePivotLocationForSelection(false);
    if (bBlueprint) ActiveBlueprint.Reset();
    GEditor->RedrawLevelEditingViewports();
}

void FOriginPivotViewportMode::Shutdown()
{
    if (GEditor) FinishEditing();
}

void FOriginPivotViewportMode::Enter()
{
    FEdMode::Enter();
}

void FOriginPivotViewportMode::Exit()
{
    if (Session) Session->Cancel();
    if (BlueprintSession) BlueprintSession->Cancel();
    Session.Reset();
    BlueprintSession.Reset();
    if (BlueprintAsset.IsValid() && ActiveBlueprint == BlueprintAsset) ActiveBlueprint.Reset();
    BlueprintAsset.Reset();
    BlueprintNode.Reset();
    BlueprintPreviewPivot.Reset();
    bTracking = false;
    bFallbackTracking = false;
    Target.Reset();
    FEdMode::Exit();
}

void FOriginPivotViewportMode::SetTarget(UOriginPivotComponent* Pivot)
{
    if (Session) Session->Cancel();
    if (BlueprintSession) BlueprintSession->Cancel();
    Session.Reset();
    BlueprintSession.Reset();
    Target = Pivot;
    UBlueprint* BP = nullptr;
    USCS_Node* Node = nullptr;
    if (OriginBlueprintPivot::Resolve(Pivot, BP, Node))
    {
        BlueprintAsset = BP;
        BlueprintNode = Node;
        // Capture a stable template baseline when entering the explicit edit
        // mode. The Blueprint viewport's newer native gizmo pipeline may start
        // moving the component before legacy FEdMode::StartTracking is called.
        BlueprintSession = MakeUnique<FOriginBlueprintPivotEditSession>(Pivot);
        USceneComponent* Preview = OriginBlueprintPivot::FindPreviewComponent(BP, Node);
        BlueprintPreviewPivot = Preview;
        InitialPivotWorld = Preview ? Preview->GetComponentTransform() : FTransform::Identity;
        LastBlueprintWidgetLocation = Preview ? Preview->GetComponentLocation() : FVector::ZeroVector;
    }
    else
    {
        BlueprintAsset.Reset();
        BlueprintNode.Reset();
        BlueprintPreviewPivot.Reset();
        InitialPivotWorld = IsValid(Pivot) ? Pivot->GetComponentTransform() : FTransform::Identity;
        LastBlueprintWidgetLocation = FVector::ZeroVector;
    }
    bExitRequested = false;
    RefreshWidget();
}

bool FOriginPivotViewportMode::IsValidTarget() const
{
    const UOriginPivotComponent* Pivot = Target.Get();
    if (!IsValid(Pivot) || !GEditor || GEditor->PlayWorld) return false;
    if (BlueprintAsset.IsValid())
    {
        UBlueprint* BP = BlueprintAsset.Get();
        // The authored SCS node and component template are stable edit targets;
        // the Blueprint preview Actor and its components are NOT. Unreal can
        // reconstruct the preview on a template edit during a gizmo drag. Do
        // not exit just because the preview component UObject was replaced.
        // This must also stay cheap: IsValidTarget() runs every viewport tick
        // and from the numeric Details value bindings.
        USCS_Node* AuthoredNode = BlueprintNode.Get();
        return IsValid(BP) && IsValid(AuthoredNode) &&
            IsValid(BP->SimpleConstructionScript) &&
            AuthoredNode->GetSCS() == BP->SimpleConstructionScript &&
            AuthoredNode->ComponentTemplate.Get() == Pivot;
    }
    const USelection* Selection = GEditor->GetSelectedComponents();
    return Pivot->IsRegistered() && Pivot->GetWorld() &&
        Pivot->GetWorld()->WorldType == EWorldType::Editor &&
        Selection && Selection->IsSelected(Pivot);
}

void FOriginPivotViewportMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
    FEdMode::Tick(ViewportClient, DeltaTime);
    ORIGIN_PERF_SCOPE(PivotViewportTick);
    // In UE 5.8 the Blueprint Viewport may dispatch its native transform gizmo
    // without calling the legacy mode's tracking pair. Only while Origin's
    // explicit Blueprint mode is active, recover tracking from a left-button
    // gizmo drag. This does not intercept RMB camera navigation or normal
    // actor/component movement outside Origin editing.
    if (BlueprintAsset.IsValid() && ViewportClient && FSlateApplication::IsInitialized())
    {
        const bool bLeftHeld = FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::LeftMouseButton);
        // The Blueprint component editor can route its interactive gizmo
        // through ITF without populating the legacy EAxisList field. Capture
        // the drag at mouse-down instead of waiting for a legacy axis or delta.
        // An unrelated click yields an empty transaction, canceled on release.
        if (!bTracking && bLeftHeld)
        {
            if (StartTracking(ViewportClient, nullptr)) bFallbackTracking = true;
        }
        else if (bFallbackTracking && bTracking && !bLeftHeld)
            EndTracking(ViewportClient, nullptr);
    }
    // A Blueprint preview may be reconstructed as a consequence of one of our
    // own gizmo deltas. Rebind instead of treating a changed preview pointer as
    // a selection change. A missing preview is tolerated transiently; InputDelta
    // simply declines to apply while the preview is unavailable.
    if (BlueprintAsset.IsValid() && BlueprintNode.IsValid())
    {
        // Blueprint preview movement sometimes bypasses FEdMode::InputDelta.
        // Reconcile against drag-start authored children before updating widget.
        if (bTracking) ReconcileBlueprintDrag();
        USceneComponent* CurrentPreview = OriginBlueprintPivot::FindPreviewComponent(BlueprintAsset.Get(), BlueprintNode.Get());
        if (CurrentPreview != BlueprintPreviewPivot.Get())
        {
            BlueprintPreviewPivot = CurrentPreview;
        }
        if (CurrentPreview) LastBlueprintWidgetLocation = CurrentPreview->GetComponentLocation();
    }
    if (!IsValidTarget())
    {
        // Selection/world changes must never leave Origin editing a stale component.
        if (Session) Session->Cancel();
        if (BlueprintSession) BlueprintSession->Cancel();
        Session.Reset();
        BlueprintSession.Reset();
        bTracking = false;
        bFallbackTracking = false;
        if (!bExitRequested)
        {
            bExitRequested = true;
            // Never activate another editor mode from within the mode manager's
            // Tick iteration. Request the normal deactivation and restore the
            // prior default mode on the next core tick after the exit completes.
            const bool bRestoreDefault = bPreviousDefaultModeActive;
            const bool bShowWidget = bPreviousWidgetVisible;
            const UE::Widget::EWidgetMode SavedWidgetMode = PreviousWidgetMode;
            UBlueprint* BP = BlueprintAsset.Get();
            const bool bBlueprint = BlueprintAsset.IsValid();
            TWeakObjectPtr<UBlueprint> WeakBP(BP);
            RequestDeletion();
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [bRestoreDefault, bShowWidget, SavedWidgetMode, bBlueprint, WeakBP, Attempts = 0](float) mutable
                {
                    if (!GEditor) return false;
                    FEditorModeTools* Manager = bBlueprint ? FindBlueprintTools(WeakBP.Get()) : &GLevelEditorModeTools();
                    if (!Manager) return false;
                    FEditorModeTools& Tools = *Manager;
                    // A deletion request can complete after this ticker callback.
                    // Give the native mode manager several frames to remove it.
                    if (Tools.IsModeActive(FOriginPivotViewportMode::ModeID)) return ++Attempts < 10;
                    if (bRestoreDefault && !Tools.IsDefaultModeActive()) Tools.ActivateDefaultMode();
                    Tools.SetWidgetMode(SavedWidgetMode);
                    Tools.SetShowWidget(bShowWidget);
                    Tools.SetCurrentWidgetAxis(EAxisList::None);
                    if (!bBlueprint && GUnrealEd) GUnrealEd->UpdatePivotLocationForSelection(false);
                    GEditor->RedrawLevelEditingViewports();
                    return false;
                }), 0.0f);
        }
    }
}

bool FOriginPivotViewportMode::UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const
{
    return IsValidTarget() &&
        (CheckMode == UE::Widget::WM_Translate || CheckMode == UE::Widget::WM_Rotate);
}

FVector FOriginPivotViewportMode::GetWidgetLocation() const
{
    if (BlueprintNode.IsValid())
    {
        if (const USceneComponent* Preview = BlueprintPreviewPivot.Get())
            return Preview->GetComponentLocation();
        // Do not render a widget at the CDO's world origin while a Blueprint
        // preview is being replaced. Keep the last known viewport position.
        return LastBlueprintWidgetLocation;
    }
    if (const UOriginPivotComponent* Pivot = Target.Get()) return Pivot->GetComponentLocation();
    return FVector::ZeroVector;
}

bool FOriginPivotViewportMode::StartTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
    if (bTracking) return true;
    if (!IsValidTarget() || !ViewportClient) return false;
    // Level Editor uses the native axis for drag eligibility. Blueprint gizmos
    // may have no legacy axis even while actively moving a component.
    if (!CanStartTracking(BlueprintAsset.IsValid(),
            ViewportClient->GetCurrentWidgetAxis() != EAxisList::None)) return false;
    if (BlueprintAsset.IsValid())
    {
        UOriginPivotComponent* Pivot = Target.Get();
        if (!BlueprintSession) BlueprintSession = MakeUnique<FOriginBlueprintPivotEditSession>(Pivot);
        // Take the source undo snapshot BEFORE Blueprint viewport input can
        // independently move the authored pivot or its preview children.
        BlueprintSession->PrepareForViewportDrag();
        UE_LOG(LogOriginPivotViewport, Verbose, TEXT("Blueprint gizmo tracking started"));
    }
    bTracking = true;
    bFallbackTracking = false;
    return true;
}

bool FOriginPivotViewportMode::InputDelta(FEditorViewportClient* ViewportClient, FViewport* Viewport,
    FVector& Drag, FRotator& Rotation, FVector& Scale)
{
    if (!ViewportClient ||
        !CanStartTracking(BlueprintAsset.IsValid(),
            ViewportClient->GetCurrentWidgetAxis() != EAxisList::None))
        return false;
    if (!bTracking && BlueprintAsset.IsValid())
    {
        if (StartTracking(ViewportClient, Viewport)) bFallbackTracking = true;
    }
    if (!bTracking) return false;
    // Never allow normal Actor/Component movement to run underneath the edit mode.
    if (!IsValidTarget()) return true;
    if (!Scale.IsNearlyZero()) return true; // scaling a pivot is unsupported
    if (Drag.IsNearlyZero() && Rotation.IsNearlyZero()) return true;

    UOriginPivotComponent* Pivot = Target.Get();
    if (BlueprintAsset.IsValid())
    {
        UE_LOG(LogOriginPivotViewport, VeryVerbose, TEXT("Blueprint FEdMode::InputDelta received"));
        USCS_Node* Node = BlueprintNode.Get();
        // Preview instances may be replaced between input events. Always use
        // the current instance, never the one cached when editing started.
        USceneComponent* Preview = OriginBlueprintPivot::FindPreviewComponent(BlueprintAsset.Get(), Node);
        if (!Preview || !Node || Node->ComponentTemplate.Get() != Pivot) return true;
        BlueprintPreviewPivot = Preview;
        FTransform CandidateWorld = Preview->GetComponentTransform();
        CandidateWorld.AddToTranslation(Drag);
        CandidateWorld.SetRotation(Rotation.Quaternion() * CandidateWorld.GetRotation());
        const AActor* PreviewOwner = Preview->GetOwner();
        if (!Preview->GetAttachParent() && !PreviewOwner) return true;
        FTransform ParentWorld = Preview->GetAttachParent()
            ? Preview->GetAttachParent()->GetComponentTransform()
            : PreviewOwner->GetActorTransform();
        const FTransform CandidateRelative = CandidateWorld.GetRelativeTransform(ParentWorld);
        if (!BlueprintSession) BlueprintSession = MakeUnique<FOriginBlueprintPivotEditSession>(Pivot);
        FText Reason;
        if (!BlueprintSession->Apply(CandidateRelative, Reason)) Origin::Notify(Reason);
        // The template edit may reconstruct the preview synchronously. The
        // next Tick/InputDelta will resolve a new instance if needed.
        if (USceneComponent* UpdatedPreview = OriginBlueprintPivot::FindPreviewComponent(BlueprintAsset.Get(), Node))
        {
            BlueprintPreviewPivot = UpdatedPreview;
            LastBlueprintWidgetLocation = UpdatedPreview->GetComponentLocation();
        }
        ViewportClient->Invalidate();
        return true;
    }
    FTransform Candidate = Pivot->GetComponentTransform();
    Candidate.AddToTranslation(Drag);
    Candidate.SetRotation(Rotation.Quaternion() * Candidate.GetRotation());
    if (!Session) Session = MakeUnique<FOriginPivotEditSession>(Pivot);
    FText Reason;
    const EOriginPivotResult Result = Session->Apply(Candidate, Reason);
    if (Result != EOriginPivotResult::Success && Result != EOriginPivotResult::NoChange)
    {
        Origin::Notify(Reason);
        // Rejected drags must not fall through to native component motion.
    }
    RefreshWidget();
    return true;
}


void FOriginPivotViewportMode::ReconcileBlueprintDrag()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Origin_BlueprintReconcile);
    if (!bTracking || !BlueprintSession || !BlueprintAsset.IsValid() || !BlueprintNode.IsValid()) return;
    UOriginPivotComponent* Pivot = Target.Get();
    USceneComponent* Preview = OriginBlueprintPivot::FindPreviewComponent(BlueprintAsset.Get(), BlueprintNode.Get());
    if (!IsValid(Pivot) || !IsValid(Preview)) return;
    const FTransform DesiredRelative = Preview->GetRelativeTransform();
    if (!BlueprintSession->NeedsReconciliation(DesiredRelative)) return;
    FText Reason;
    if (!BlueprintSession->Apply(DesiredRelative, Reason))
    {
        UE_LOG(LogOriginPivotViewport, Warning, TEXT("Blueprint gizmo reconciliation rejected: %s"), *Reason.ToString());
    }
    else
    {
        BlueprintPreviewPivot = OriginBlueprintPivot::FindPreviewComponent(BlueprintAsset.Get(), BlueprintNode.Get());
        if (USceneComponent* Updated = BlueprintPreviewPivot.Get()) LastBlueprintWidgetLocation = Updated->GetComponentLocation();
        UE_LOG(LogOriginPivotViewport, VeryVerbose, TEXT("Blueprint preview children reconciled against drag-start transforms"));
    }
}

bool FOriginPivotViewportMode::EndTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
    if (!bTracking) return false;
    if (BlueprintAsset.IsValid())
    {
        ReconcileBlueprintDrag();
        UE_LOG(LogOriginPivotViewport, Verbose, TEXT("Blueprint gizmo tracking ended; source changes reconciled: %s"),
            BlueprintSession && BlueprintSession->HasChanged() ? TEXT("Yes") : TEXT("No"));
    }
    if (Session) Session->Finish();
    if (BlueprintSession) BlueprintSession->Finish();
    Session.Reset();
    BlueprintSession.Reset();
    bTracking = false;
    bFallbackTracking = false;
    RefreshWidget();
    return true;
}

bool FOriginPivotViewportMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
    FKey Key, EInputEvent Event)
{
    if (Event == IE_Pressed && Key == EKeys::Escape)
    {
        if (Session) Session->Cancel();
        if (BlueprintSession) BlueprintSession->Cancel();
        Session.Reset();
        BlueprintSession.Reset();
        bTracking = false;
        bFallbackTracking = false;
        RefreshWidget();
        FinishEditing();
        return true;
    }
    return FEdMode::InputKey(ViewportClient, Viewport, Key, Event);
}

void FOriginPivotViewportMode::RefreshWidget() const
{
    if (!GEditor) return;
    if (BlueprintAsset.IsValid()) return; // Preview viewport invalidated by InputDelta.
    const USelection* Selection = GEditor->GetSelectedComponents();
    if (GUnrealEd && Target.IsValid() && Selection && Selection->IsSelected(Target.Get()))
    {
        GUnrealEd->UpdatePivotLocationForSelection(false);
    }
    GEditor->RedrawLevelEditingViewports();
}
