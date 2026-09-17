#include "OriginHierarchy.h"
#include "OriginOperations.h"
#include "SSceneOutliner.h"
#include "ActorTreeItem.h"
#include "FolderTreeItem.h"
#include "EditorActorFolders.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyleRegistry.h"

FOriginHierarchy::FOriginHierarchy(ISceneOutliner& InOutliner)
    : Outliner(StaticCastSharedRef<SSceneOutliner>(InOutliner.AsShared())) {}
void FOriginHierarchy::Rebuild()
{
    Graph.Reset(); Children.Reset(); DescendantCounts.Reset();
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || World->WorldType != EWorldType::Editor) return;
    FActorFolders::Get().ForEachFolder(*World,[this](const FFolder& Folder) {
        auto Item = MakeShared<FFolderTreeItem>(Folder);
        FNode Node; Node.Item = Item;
        const FFolder Parent = Folder.GetParent();
        Node.Parent = FFolderTreeItem(Parent).GetID(); // Empty path is a virtual root for sibling lookup.
        Graph.Add(Item->GetID(), MoveTemp(Node));
        return true;
    });
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Origin::IsEditorActor(Actor)) continue;
        auto Item = MakeShared<FActorTreeItem>(Actor);
        FNode Node; Node.Item = Item;
        if (AActor* Parent = Actor->GetAttachParentActor()) Node.Parent = FActorTreeItem(Parent).GetID();
        else Node.Parent = FFolderTreeItem(Actor->GetFolder()).GetID();
        Graph.Add(Item->GetID(), MoveTemp(Node));
    }
    TMap<FSceneOutlinerTreeItemID,int32> Remaining;
    for (const auto& Pair : Graph)
    {
        Remaining.Add(Pair.Key,0); DescendantCounts.Add(Pair.Key,0);
        if (Pair.Value.Parent.IsSet())
            Children.Add(Pair.Value.Parent.GetValue(),Pair.Key);
    }
    for (const auto& Pair : Children) if (int32* Count = Remaining.Find(Pair.Key)) ++*Count;
    TArray<FSceneOutlinerTreeItemID> Leaves;
    for (const auto& Pair : Remaining) if (Pair.Value == 0) Leaves.Add(Pair.Key);
    while (!Leaves.IsEmpty())
    {
        const auto Leaf = Leaves.Pop(EAllowShrinking::No);
        const auto& Parent = Graph.FindChecked(Leaf).Parent;
        if (Parent.IsSet() && Remaining.Contains(Parent.GetValue()))
        {
            DescendantCounts.FindChecked(Parent.GetValue()) += 1 + DescendantCounts.FindChecked(Leaf);
            if (--Remaining.FindChecked(Parent.GetValue()) == 0) Leaves.Add(Parent.GetValue());
        }
    }
}
TArray<FSceneOutlinerTreeItemID> FOriginHierarchy::Targets(FSceneOutlinerTreeItemID ID, int32 Action) const
{
    TArray<FSceneOutlinerTreeItemID> Result;
    const FNode* Node = Graph.Find(ID);
    if (!Node) return Result;
    if (Action == 2)
    {
        if (Node->Parent.IsSet() && Graph.Contains(Node->Parent.GetValue())) Result.Add(Node->Parent.GetValue());
    }
    else if (Action == 3)
    {
        if (Node->Parent.IsSet()) Children.MultiFind(Node->Parent.GetValue(),Result);
        Result.Remove(ID);
    }
    else
    {
        TArray<FSceneOutlinerTreeItemID> Pending; Pending.Add(ID);
        TSet<FSceneOutlinerTreeItemID> Seen; Seen.Add(ID);
        while (!Pending.IsEmpty())
        {
            const auto Parent = Pending.Pop(EAllowShrinking::No);
            TArray<FSceneOutlinerTreeItemID> Direct; Children.MultiFind(Parent,Direct);
            for (const auto& Child : Direct) if (!Seen.Contains(Child))
            {
                Seen.Add(Child); Result.Add(Child);
                if (Action == 1) Pending.Add(Child);
            }
        }
    }
    return Result;
}
FReply FOriginHierarchy::Select(FSceneOutlinerTreeItemID ID, int32 Action)
{
    auto View = Outliner.Pin();
    if (!View || !GEditor || GEditor->PlayWorld) return FReply::Handled();
    Rebuild();
    const auto Result = Targets(ID, Action);
    if (Result.IsEmpty()) return FReply::Handled();
    const bool Add = FSlateApplication::Get().GetModifierKeys().IsShiftDown();
    // Build the entire desired selection first. Do not call SelectActor or
    // NoteSelectionChange here: their legacy notifications clear folder rows
    // outside SSceneOutliner's native reentrancy guard.
    TArray<FSceneOutlinerTreeItemPtr> FinalRows;
    TSet<FSceneOutlinerTreeItemID> AddedIDs;
    auto AddRow=[&](const FSceneOutlinerTreeItemPtr& Item)
    {
        if(Item.IsValid() && !AddedIDs.Contains(Item->GetID()))
        { AddedIDs.Add(Item->GetID()); FinalRows.Add(Item); }
    };
    if(Add)
    {
        for(const auto& Item:View->GetSelectedItems()) AddRow(Item);
        // Include selected loaded actors hidden by the Outliner filter. Native
        // actor browsing reads actors from this selection, not only visible rows.
        for(AActor* Actor:Origin::SelectedActors())
        {
            auto Item=View->GetTreeItem(Actor);
            if(Item.IsValid()) AddRow(Item);
            else AddRow(MakeShared<FActorTreeItem>(Actor));
        }
    }
    int32 HiddenFolders=0;
    FSceneOutlinerTreeItemPtr ParentFolder;
    for(const auto& Target:Result)
    {
        if(auto Item=View->GetTreeItem(Target))
        {
            AddRow(Item);
            if(Action==2 && Item->IsA<FFolderTreeItem>()) ParentFolder=Item;
        }
        else if(const FNode* Node=Graph.Find(Target))
        {
            if(Node->Item.IsValid() && Node->Item->IsA<FActorTreeItem>()) AddRow(Node->Item);
            else ++HiddenFolders;
        }
    }
    if(FinalRows.IsEmpty())
    {
        if(HiddenFolders) Origin::Notify(FText::FromString(TEXT("Clear the Outliner filter to select the matching folder rows.")));
        return FReply::Handled();
    }
    // The array overload in UE 5.8 finishes with ESelectInfo::Direct, even when
    // passed OnMouseClick. Stage the full list directly, then use the SINGLE
    // item overload to issue one real selection event without clearing the list.
    View->SetItemSelection(FinalRows,true,ESelectInfo::Direct);
    View->AddToSelection(FinalRows.Last(),ESelectInfo::OnMouseClick);
    // FActorBrowsingMode now updates the typed-element selection within the
    // Outliner's guard, preserving folders as part of this same operation.
    if(ParentFolder.IsValid()) View->ScrollItemIntoView(ParentFolder);
    if(HiddenFolders) Origin::Notify(FText::FromString(TEXT("Matching loaded actors were selected. Clear the Outliner filter to also select folder rows hidden by that filter.")));
    return FReply::Handled();
}
TSharedRef<SWidget> FOriginHierarchy::Button(const TSharedRef<FRow>& Row, int32 Action)
{
    static const TCHAR* Names[] = {TEXT("Immediate children"),TEXT("All descendants"),TEXT("Parent"),TEXT("Siblings")};
    static const TCHAR* Brushes[] = {TEXT("Origin.Children"),TEXT("Origin.Descendants"),TEXT("Origin.Parent"),TEXT("Origin.Siblings")};
    const auto* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("OriginStyle"));
    const TWeakPtr<FOriginHierarchy> Weak = StaticCastSharedRef<FOriginHierarchy>(AsShared());
    // Keep the combined hit area at 20x20. Smaller, proportional artwork
    // leaves more vertical space between rows without shrinking click targets.
    // Each half contributes 0.5 Slate units at the seam: one unit total.
    const FMargin OuterPadding = Action == 0 ? FMargin(3.f,2.5f,3.f,0.5f) :
        Action == 1 ? FMargin(3.f,0.5f,3.f,2.5f) :
        Action == 2 ? FMargin(2.5f,3.f,0.5f,3.f) : FMargin(0.5f,3.f,2.5f,3.f);
    return SNew(SBox).WidthOverride(Action < 2 ? 20.f : 10.f).HeightOverride(Action < 2 ? 10.f : 20.f)
    [SNew(SButton).ButtonStyle(FAppStyle::Get(),TEXT("SimpleButton")).ContentPadding(OuterPadding)
        // SButton adds style padding to ContentPadding. Override both states
        // so the style cannot squeeze the half-icon or shift it when pressed.
        .NormalPaddingOverride(FMargin(0)).PressedPaddingOverride(FMargin(0))
        .HAlign(HAlign_Center).VAlign(VAlign_Center)
        .IsEnabled_Lambda([Row,Action] { return Row->Counts[Action] > 0; })
        .ToolTipText_Lambda([Row,Action] { return FText::FromString(FString(Names[Action]) +
            (Row->Counts[Action] ? TEXT("\nClick to select. Shift-click adds to the selection. Loaded actors only.") : TEXT("\nNo matching selectable items."))); })
        .OnClicked_Lambda([Weak,Row,Action] { if(auto Self=Weak.Pin()) return Self->Select(Row->ID,Action); return FReply::Handled(); })
        [SNew(SBox).WidthOverride(Action < 2 ? 14.f : 7.f).HeightOverride(Action < 2 ? 7.f : 14.f)
            [SNew(SImage).Image(Style->GetBrush(Brushes[Action])).ColorAndOpacity(FSlateColor::UseForeground())]]];
}
SHeaderRow::FColumn::FArguments FOriginHierarchy::ConstructHeaderRowColumn()
{
    return SHeaderRow::Column(ID()).DefaultLabel(FText::FromString(TEXT("Origin"))).ManualWidth(52.f);
}
const TSharedRef<SWidget> FOriginHierarchy::ConstructRowWidget(FSceneOutlinerTreeItemRef Item, const STableRow<FSceneOutlinerTreeItemPtr>&)
{
    if (!Item->IsA<FActorTreeItem>() && !Item->IsA<FFolderTreeItem>()) return SNullWidget::NullWidget;
    auto State = MakeShared<FRow>(Item->GetID()); Rows.Add(State); NextUpdate = 0;
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2,0,4,0)
        [SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[Button(State,0)]
            + SVerticalBox::Slot().AutoHeight()[Button(State,1)]]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[Button(State,2)]
            + SHorizontalBox::Slot().AutoWidth()[Button(State,3)]];
}
void FOriginHierarchy::Tick(double Now, float)
{
    if (Now < NextUpdate) return;
    NextUpdate = Now + .5;
    Rows.RemoveAll([](const TWeakPtr<FRow>& R) { return !R.IsValid(); });
    if (Rows.IsEmpty()) return;
    Rebuild();
    for (auto& Weak : Rows) if(auto Row=Weak.Pin())
    {
        Row->Counts[0] = Children.Num(Row->ID);
        Row->Counts[1] = DescendantCounts.FindRef(Row->ID);
        const FNode* Node = Graph.Find(Row->ID);
        const bool HasParent = Node && Node->Parent.IsSet() && Graph.Contains(Node->Parent.GetValue());
        Row->Counts[2] = HasParent ? 1 : 0;
        Row->Counts[3] = Node && Node->Parent.IsSet() ? FMath::Max(0,Children.Num(Node->Parent.GetValue()) - 1) : 0;
    }
}
