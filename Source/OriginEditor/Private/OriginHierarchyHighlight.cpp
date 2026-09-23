#include "OriginHierarchyHighlight.h"

#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWindow.h"

FOriginHierarchyHighlight::~FOriginHierarchyHighlight()
{
    Shutdown();
}

TSharedPtr<SWindow> FOriginHierarchyHighlight::CreateStripWindow()
{
    if (!FSlateApplication::IsInitialized())
    {
        return nullptr;
    }

    TSharedPtr<SWindow> NewWindow = SNew(SWindow)
        .Type(EWindowType::ToolTip)
        .SizingRule(ESizingRule::FixedSize)
        .ClientSize(FVector2D(1.0f, 1.0f))
        .CreateTitleBar(false)
        .SupportsMinimize(false)
        .SupportsMaximize(false)
        .IsPopupWindow(true)
        .IsTopmostWindow(true)
        .FocusWhenFirstShown(false)
        .ActivationPolicy(EWindowActivationPolicy::Never)
        .UseOSWindowBorder(false)
        [
            SNew(SBorder)
            .Padding(0.0f)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor_Lambda([this]() { return CurrentColor; })
        ];

    NewWindow->SetAcceptsInput(false);
    FSlateApplication::Get().AddWindow(NewWindow.ToSharedRef(), false);
    return NewWindow;
}

void FOriginHierarchyHighlight::EnsureWindows()
{
    if (!FSlateApplication::IsInitialized())
    {
        return;
    }

    if (!TopWindow.IsValid())
    {
        TopWindow = CreateStripWindow();
    }
    if (!BottomWindow.IsValid())
    {
        BottomWindow = CreateStripWindow();
    }
    if (!LeftWindow.IsValid())
    {
        LeftWindow = CreateStripWindow();
    }
    if (!RightWindow.IsValid())
    {
        RightWindow = CreateStripWindow();
    }
}

void FOriginHierarchyHighlight::Show(const FGeometry& RowGeometry, const FLinearColor& Color)
{
    EnsureWindows();
    if (!TopWindow.IsValid() || !BottomWindow.IsValid() || !LeftWindow.IsValid() || !RightWindow.IsValid())
    {
        return;
    }

    CurrentColor = Color;

    const FVector2D Position = RowGeometry.GetAbsolutePosition();
    const FVector2D Size = RowGeometry.GetAbsoluteSize();
    const float Width = FMath::Max(1.0f, Size.X);
    const float Height = FMath::Max(1.0f, Size.Y);
    const float Thickness = FMath::Min(2.0f, FMath::Min(Width, Height));
    const float InnerHeight = FMath::Max(1.0f, Height - (Thickness * 2.0f));

    // Use four narrow popup strips instead of one full-row popup. A full-row
    // popup can visually occlude the native Outliner content even when its
    // center is transparent. Edge-only windows leave Unreal's row completely
    // uncovered while still providing strong hierarchy feedback.
    TopWindow->ReshapeWindow(
        Position,
        FVector2D(Width, Thickness));

    BottomWindow->ReshapeWindow(
        FVector2D(Position.X, Position.Y + Height - Thickness),
        FVector2D(Width, Thickness));

    LeftWindow->ReshapeWindow(
        FVector2D(Position.X, Position.Y + Thickness),
        FVector2D(Thickness, InnerHeight));

    RightWindow->ReshapeWindow(
        FVector2D(Position.X + Width - Thickness, Position.Y + Thickness),
        FVector2D(Thickness, InnerHeight));

    TopWindow->ShowWindow();
    BottomWindow->ShowWindow();
    LeftWindow->ShowWindow();
    RightWindow->ShowWindow();
}

void FOriginHierarchyHighlight::Hide()
{
    if (TopWindow.IsValid())
    {
        TopWindow->HideWindow();
    }
    if (BottomWindow.IsValid())
    {
        BottomWindow->HideWindow();
    }
    if (LeftWindow.IsValid())
    {
        LeftWindow->HideWindow();
    }
    if (RightWindow.IsValid())
    {
        RightWindow->HideWindow();
    }
}

void FOriginHierarchyHighlight::Shutdown()
{
    if (FSlateApplication::IsInitialized())
    {
        if (TopWindow.IsValid())
        {
            FSlateApplication::Get().RequestDestroyWindow(TopWindow.ToSharedRef());
        }
        if (BottomWindow.IsValid())
        {
            FSlateApplication::Get().RequestDestroyWindow(BottomWindow.ToSharedRef());
        }
        if (LeftWindow.IsValid())
        {
            FSlateApplication::Get().RequestDestroyWindow(LeftWindow.ToSharedRef());
        }
        if (RightWindow.IsValid())
        {
            FSlateApplication::Get().RequestDestroyWindow(RightWindow.ToSharedRef());
        }
    }

    TopWindow.Reset();
    BottomWindow.Reset();
    LeftWindow.Reset();
    RightWindow.Reset();
}
