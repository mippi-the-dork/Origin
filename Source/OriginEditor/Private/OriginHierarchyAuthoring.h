// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UOriginPivotComponent;
class USCS_Node;
class UBlueprint;

/** Structural operations on the Blueprint's authored SCS nodes, never preview-only objects.
 * A single operation is one native editor transaction, with preservation preflight before mutation.
 */
struct FOriginHierarchyAuthoring
{
    static bool CanEdit(UOriginPivotComponent* Pivot);
    /** The pivot and sibling must have the same authored SCS parent. */
    static bool AdoptSibling(UOriginPivotComponent* Pivot, FName SiblingVariable, FText& OutReason);
    /** Move one authored direct child of the pivot up to the pivot's SCS parent. */
    static bool PromoteChild(UOriginPivotComponent* Pivot, FName ChildVariable, FText& OutReason);
    /** Insert a new Origin Pivot immediately above one authored direct child. */
    static bool InsertPivotAboveChild(UOriginPivotComponent* Pivot, FName ChildVariable,
        FName& OutNewPivotVariable, FText& OutReason);
    /** Read-only options; never modify the asset or its preview. */
    static void ListSiblingsAndChildren(UOriginPivotComponent* Pivot,
        TArray<FName>& OutSiblings, TArray<FName>& OutChildren);
};
