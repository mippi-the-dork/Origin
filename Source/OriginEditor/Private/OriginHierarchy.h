// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "ISceneOutlinerColumn.h"
#include "SceneOutlinerStandaloneTypes.h"
class SSceneOutliner;
class FOriginHierarchy final : public ISceneOutlinerColumn
{
    struct FNode
    {
        FSceneOutlinerTreeItemPtr Item;
        TOptional<FSceneOutlinerTreeItemID> Parent;
    };
    struct FRow
    {
        FSceneOutlinerTreeItemID ID;
        int32 Counts[4] = {0,0,0,0};
        explicit FRow(FSceneOutlinerTreeItemID InID) : ID(InID) {}
    };
    TWeakPtr<SSceneOutliner> Outliner;
    TMap<FSceneOutlinerTreeItemID,FNode> Graph;
    TMultiMap<FSceneOutlinerTreeItemID,FSceneOutlinerTreeItemID> Children;
    TMap<FSceneOutlinerTreeItemID,int32> DescendantCounts;
    TArray<TWeakPtr<FRow>> Rows;
    double NextUpdate = 0;
    void Rebuild();
    TArray<FSceneOutlinerTreeItemID> Targets(FSceneOutlinerTreeItemID ID, int32 Action) const;
    FReply Select(FSceneOutlinerTreeItemID ID, int32 Action);
    TSharedRef<SWidget> Button(const TSharedRef<FRow>& Row, int32 Action);
public:
    explicit FOriginHierarchy(ISceneOutliner& InOutliner);
    static FName ID() { return TEXT("Origin.Hierarchy"); }
    virtual FName GetColumnID() override { return ID(); }
    virtual SHeaderRow::FColumn::FArguments ConstructHeaderRowColumn() override;
    virtual const TSharedRef<SWidget> ConstructRowWidget(FSceneOutlinerTreeItemRef Item, const STableRow<FSceneOutlinerTreeItemPtr>& Row) override;
    virtual void Tick(double Now, float Delta) override;
};
