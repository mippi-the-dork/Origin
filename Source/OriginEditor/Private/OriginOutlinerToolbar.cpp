// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginOutlinerToolbar.h"
#include "OriginOperations.h"
#include "SSceneOutliner.h"
#include "Editor.h"
#include "Layout/Children.h"
#include "Widgets/SWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyleRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogOriginOutlinerToolbar, Log, All);

namespace OriginOutlinerToolbar
{
namespace
{
    const FName SearchType(TEXT("SFilterSearchBox"));
    const FName HorizontalBoxType(TEXT("SHorizontalBox"));
    const FName FolderButtonType(TEXT("SButton"));
    const FName OriginButtonTag(TEXT("Origin.CreateAnchor.OutlinerToolbar"));

    // Find the *specific* horizontal box Unreal uses for the Outliner search,
    // New Folder and View Options controls. We don't traverse/modify any
    // actor rows or the browsing mode's hierarchy.
    TSharedPtr<SHorizontalBox> FindNativeToolbar(const TSharedRef<SWidget>& Widget)
    {
        FChildren* Children = Widget->GetChildren();
        if (!Children)
        {
            return nullptr;
        }

        if (Widget->GetType() == HorizontalBoxType)
        {
            for (int32 Index = 0; Index < Children->Num(); ++Index)
            {
                if (Children->GetChildAt(Index)->GetType() == SearchType)
                {
                    return StaticCastSharedRef<SHorizontalBox>(Widget);
                }
            }
        }

        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            if (TSharedPtr<SHorizontalBox> Result = FindNativeToolbar(Children->GetChildAt(Index)))
            {
                return Result;
            }
        }
        return nullptr;
    }

    bool InsertBeforeNewFolder(const TSharedRef<SHorizontalBox>& Toolbar)
    {
        FChildren* Children = Toolbar->GetChildren();
        if (!Children)
        {
            return false;
        }

        int32 SearchIndex = INDEX_NONE;
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
            if (Child->GetTag() == OriginButtonTag)
            {
                return true; // Already inserted. Column refreshes must not duplicate it.
            }
            if (Child->GetType() == SearchType)
            {
                SearchIndex = Index;
            }
        }

        // In Unreal 5.8, the first direct SButton after the search field is
        // New Folder. If this isn't true (such as in a customized Outliner),
        // do nothing rather than inserting into an unknown control layout.
        if (SearchIndex == INDEX_NONE || SearchIndex + 1 >= Children->Num())
        {
            return false;
        }

        const int32 FolderIndex = SearchIndex + 1;
        if (Children->GetChildAt(FolderIndex)->GetType() != FolderButtonType)
        {
            return false;
        }

        const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("OriginStyle"));
        const FSlateBrush* Icon = Style ? Style->GetBrush(TEXT("Origin.AnchorAdd")) :
            FAppStyle::Get().GetBrush(TEXT("Icons.Plus"));

        TSharedRef<SButton> Button = SNew(SButton)
            .ButtonStyle(FAppStyle::Get(), TEXT("SimpleButton"))
            .ToolTipText(FText::FromString(TEXT("Create Origin Anchor\nParent selected actors beneath an anchor without changing their world transforms. With no selection, create an empty anchor at world origin.")))
            .IsEnabled_Lambda([] { return GEditor && !GEditor->PlayWorld; })
            .OnClicked_Lambda([] { Origin::CreateAnchor(); return FReply::Handled(); })
            [
                SNew(SImage)
                .ColorAndOpacity(FSlateColor::UseForeground())
                .Image(Icon)
            ];
        Button->SetTag(OriginButtonTag);

        Toolbar->InsertSlot(FolderIndex)
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.f, 0.f, 0.f, 0.f)
            [ Button ];
        return true;
    }
}

void Schedule(const TSharedRef<SSceneOutliner>& Outliner)
{
    // Our column is created during SSceneOutliner::Construct, BEFORE Unreal
    // builds the toolbar. Install on the next Slate active-timer tick once the
    // native toolbar exists, then immediately stop the timer.
    const TWeakPtr<SSceneOutliner> WeakOutliner(Outliner);
    Outliner->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda(
        [WeakOutliner, Attempts = 0](double, float) mutable -> EActiveTimerReturnType
        {
            TSharedPtr<SSceneOutliner> View = WeakOutliner.Pin();
            if (!View.IsValid())
            {
                return EActiveTimerReturnType::Stop;
            }
            if (TSharedPtr<SHorizontalBox> Toolbar = FindNativeToolbar(View.ToSharedRef()))
            {
                if (!InsertBeforeNewFolder(Toolbar.ToSharedRef()))
                {
                    UE_LOG(LogOriginOutlinerToolbar, Verbose,
                        TEXT("Could not identify the native New Folder control. Skipping toolbar insertion to preserve the World Outliner."));
                }
                return EActiveTimerReturnType::Stop;
            }
            // Bound the retries in case Unreal changes construction timing.
            if (++Attempts >= 4)
            {
                UE_LOG(LogOriginOutlinerToolbar, Verbose,
                    TEXT("World Outliner toolbar not found. Origin did not modify the widget tree."));
                return EActiveTimerReturnType::Stop;
            }
            return EActiveTimerReturnType::Continue;
        }));
}
}
