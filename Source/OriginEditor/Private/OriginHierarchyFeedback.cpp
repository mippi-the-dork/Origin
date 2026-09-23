#include "OriginHierarchyFeedback.h"

#include "ActorFolderTreeItem.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/DragAndDrop.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "ISceneOutliner.h"
#include "ISceneOutlinerMode.h"
#include "Layout/WidgetPath.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "OriginHierarchyHighlight.h"
#include "OriginSettings.h"
#include "SceneOutlinerDragDrop.h"
#include "SceneOutlinerStandaloneTypes.h"
#include "Widgets/Views/STableRow.h"
#include "SSceneOutliner.h"

namespace OriginHierarchyFeedback
{
    static TArray<TWeakPtr<ISceneOutliner>> Outliners;

    enum class EHierarchyFeedback : uint8
    {
        None,
        Parent,
        Deparent
    };

    struct FResolvedFeedback
    {
        TSharedPtr<STableRow<FSceneOutlinerTreeItemPtr>> Row;
        EHierarchyFeedback Feedback = EHierarchyFeedback::None;

        bool IsValid() const
        {
            return Row.IsValid() && Feedback != EHierarchyFeedback::None;
        }
    };

    static void UnregisterDeadOutliners()
    {
        Outliners.RemoveAll([](const TWeakPtr<ISceneOutliner>& WeakOutliner)
        {
            return !WeakOutliner.IsValid();
        });
    }

    static bool PathContains(const FWidgetPath& Path, const SWidget* Widget)
    {
        if (!Widget)
        {
            return false;
        }

        for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
        {
            if (&Path.Widgets[Index].Widget.Get() == Widget)
            {
                return true;
            }
        }
        return false;
    }

    static bool IsIndexModifierDown(const FString& ConfiguredModifier, const FPointerEvent& PointerEvent)
    {
        FString Modifier = ConfiguredModifier;
        Modifier.TrimStartAndEndInline();
        Modifier.ToLowerInline();

        // Enum config values can be serialized either by display/name text or by
        // their underlying numeric value. Index currently maps Alt=0, Shift=1,
        // Ctrl=2, None=3. Supporting both keeps this optional integration robust
        // without introducing a compile-time dependency on Index.
        if (Modifier == TEXT("0") || Modifier.EndsWith(TEXT("alt")))
        {
            return PointerEvent.IsAltDown();
        }
        if (Modifier == TEXT("1") || Modifier.EndsWith(TEXT("shift")))
        {
            return PointerEvent.IsShiftDown();
        }
        if (Modifier == TEXT("2") || Modifier.EndsWith(TEXT("ctrl")))
        {
            return PointerEvent.IsControlDown();
        }
        return false;
    }

    static bool IsReservedForIndexReorder(
        const FGeometry& RowGeometry,
        const FPointerEvent& PointerEvent)
    {
        // Origin has no hard dependency on Index. If Index happens to be loaded,
        // mirror its public Project Settings so the two plugins never present
        // competing drag feedback for the same pointer position.
        if (!FModuleManager::Get().IsModuleLoaded(FName(TEXT("Index"))))
        {
            return false;
        }

        bool bEnableCustomOrdering = true;
        float EdgeThresholdPercent = 10.0f;
        FString ReorderModifierKey(TEXT("Alt"));

        if (GConfig)
        {
            static const TCHAR* Section = TEXT("/Script/Index.IndexSettings");
            GConfig->GetBool(Section, TEXT("bEnableCustomOrdering"), bEnableCustomOrdering, GEditorIni);
            GConfig->GetFloat(Section, TEXT("EdgeThresholdPercent"), EdgeThresholdPercent, GEditorIni);
            GConfig->GetString(Section, TEXT("ReorderModifierKey"), ReorderModifierKey, GEditorIni);
        }

        if (!bEnableCustomOrdering)
        {
            return false;
        }

        if (IsIndexModifierDown(ReorderModifierKey, PointerEvent))
        {
            return true;
        }

        const float Height = RowGeometry.GetLocalSize().Y;
        if (Height <= KINDA_SMALL_NUMBER)
        {
            return false;
        }

        const FVector2D LocalPosition = RowGeometry.AbsoluteToLocal(PointerEvent.GetScreenSpacePosition());
        const float Fraction = FMath::Clamp(LocalPosition.Y / Height, 0.0f, 1.0f);
        const float EdgeFraction = FMath::Clamp(EdgeThresholdPercent / 100.0f, 0.01f, 0.45f);
        return Fraction <= EdgeFraction || Fraction >= 1.0f - EdgeFraction;
    }

    static FResolvedFeedback ResolveFeedback(
        FSlateApplication& SlateApplication,
        const FPointerEvent& PointerEvent,
        const TSharedPtr<FDragDropOperation>& Operation)
    {
        FResolvedFeedback Result;
        if (!Operation)
        {
            return Result;
        }

        UnregisterDeadOutliners();
        const FWidgetPath Path = SlateApplication.LocateWindowUnderMouse(
            PointerEvent.GetScreenSpacePosition(),
            SlateApplication.GetInteractiveTopLevelWindows(),
            false,
            PointerEvent.GetUserIndex());

        for (const TWeakPtr<ISceneOutliner>& WeakOutliner : Outliners)
        {
            const TSharedPtr<ISceneOutliner> Outliner = WeakOutliner.Pin();
            if (!Outliner || !Outliner->GetMode())
            {
                continue;
            }

            const TSharedPtr<SSceneOutliner> OutlinerWidget =
                StaticCastSharedPtr<SSceneOutliner>(Outliner);
            if (!PathContains(Path, OutlinerWidget.Get()))
            {
                continue;
            }

            TSharedPtr<STableRow<FSceneOutlinerTreeItemPtr>> Row;
            for (int32 Index = Path.Widgets.Num() - 1; Index >= 0; --Index)
            {
                const TSharedRef<SWidget> Widget = Path.Widgets[Index].Widget;
                if (Widget->GetType() == FName(TEXT("SSceneOutlinerTreeRow")))
                {
                    Row = StaticCastSharedRef<STableRow<FSceneOutlinerTreeItemPtr>>(Widget);
                    break;
                }
            }
            if (!Row)
            {
                continue;
            }

            // Index owns the edge zones used for manual Before/After reordering,
            // plus the full row while its force-reorder modifier is held. Suppress
            // Origin hierarchy feedback there so only one drag intent is
            // communicated at a time.
            if (IsReservedForIndexReorder(Row->GetCachedGeometry(), PointerEvent))
            {
                continue;
            }

            const FSceneOutlinerTreeItemPtr* ItemPtr = Outliner->GetTree().ItemFromWidget(Row.Get());
            if (!ItemPtr || !*ItemPtr)
            {
                continue;
            }

            FSceneOutlinerDragDropPayload Payload(*Operation);
            if (!Outliner->GetMode()->ParseDragDrop(Payload, *Operation))
            {
                continue;
            }

            const FSceneOutlinerDragValidationInfo Validation =
                Outliner->GetMode()->ValidateDrop(**ItemPtr, Payload);

            switch (Validation.CompatibilityType)
            {
            case ESceneOutlinerDropCompatibility::CompatibleAttach:
            case ESceneOutlinerDropCompatibility::CompatibleMultipleAttach:
                Result.Feedback = EHierarchyFeedback::Parent;
                break;

            case ESceneOutlinerDropCompatibility::CompatibleDetach:
            case ESceneOutlinerDropCompatibility::CompatibleMultipleDetach:
                Result.Feedback = EHierarchyFeedback::Deparent;
                break;

            default:
                // Folder center-drops use Unreal's generic valid-drop state rather
                // than actor attach compatibility. A valid folder target means the
                // dragged rows will move into that folder/container.
                if (Validation.IsValid()
                    && (*ItemPtr)->CastTo<FActorFolderTreeItem>() != nullptr)
                {
                    Result.Feedback = EHierarchyFeedback::Parent;
                }
                break;
            }

            if (Result.Feedback != EHierarchyFeedback::None)
            {
                Result.Row = Row;
                return Result;
            }
        }

        return Result;
    }

    class FOriginHierarchyInputProcessor final : public IInputProcessor
    {
    public:
        virtual ~FOriginHierarchyInputProcessor() override
        {
            Highlight.Shutdown();
        }

        virtual void Tick(
            const float,
            FSlateApplication& SlateApplication,
            TSharedRef<ICursor>) override
        {
            // IInputProcessor::Tick is pure virtual in UE 5.8. Keep the visual
            // state clean if a drag ends without a mouse-button-up callback.
            if (!SlateApplication.GetDragDroppingContent())
            {
                Highlight.Hide();
            }
        }

        virtual bool HandleMouseMoveEvent(
            FSlateApplication& SlateApplication,
            const FPointerEvent& MouseEvent) override
        {
            const TSharedPtr<FDragDropOperation> Operation =
                SlateApplication.GetDragDroppingContent();
            if (!Operation)
            {
                Highlight.Hide();
                return false;
            }

            const FResolvedFeedback Feedback = ResolveFeedback(
                SlateApplication,
                MouseEvent,
                Operation);
            if (!Feedback.IsValid())
            {
                Highlight.Hide();
                return false;
            }

            const UOriginSettings* Settings = UOriginSettings::Get();
            Highlight.Show(
                Feedback.Row->GetCachedGeometry(),
                Feedback.Feedback == EHierarchyFeedback::Deparent
                    ? Settings->DeparentHighlightColor
                    : Settings->ParentHighlightColor);
            return false;
        }

        virtual bool HandleMouseButtonUpEvent(
            FSlateApplication&,
            const FPointerEvent& MouseEvent) override
        {
            if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
            {
                Highlight.Hide();
            }
            return false;
        }

        virtual bool HandleKeyDownEvent(
            FSlateApplication&,
            const FKeyEvent& KeyEvent) override
        {
            if (KeyEvent.GetKey() == EKeys::Escape)
            {
                Highlight.Hide();
            }
            return false;
        }

        void Shutdown()
        {
            Highlight.Shutdown();
        }

    private:
        FOriginHierarchyHighlight Highlight;
    };

    void RegisterOutliner(ISceneOutliner& Outliner)
    {
        const TWeakPtr<ISceneOutliner> WeakOutliner =
            StaticCastSharedRef<ISceneOutliner>(Outliner.AsShared());
        Outliners.AddUnique(WeakOutliner);
    }

    TSharedRef<IInputProcessor> CreateInputProcessor()
    {
        return MakeShared<FOriginHierarchyInputProcessor>();
    }

    void Shutdown()
    {
        Outliners.Reset();
    }
}
