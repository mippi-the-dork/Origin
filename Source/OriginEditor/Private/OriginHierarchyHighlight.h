#pragma once

#include "CoreMinimal.h"

class SWindow;

class FOriginHierarchyHighlight
{
public:
    ~FOriginHierarchyHighlight();

    void Show(const FGeometry& RowGeometry, const FLinearColor& Color);
    void Hide();
    void Shutdown();

private:
    void EnsureWindows();
    TSharedPtr<SWindow> CreateStripWindow();

    TSharedPtr<SWindow> TopWindow;
    TSharedPtr<SWindow> BottomWindow;
    TSharedPtr<SWindow> LeftWindow;
    TSharedPtr<SWindow> RightWindow;
    FLinearColor CurrentColor = FLinearColor::White;
};
