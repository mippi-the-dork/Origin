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
    /** True when this authored SCS root Pivot shares an unchanged native/inherited
     * Scene Component parent with other authored root SCS nodes (e.g. ACharacter::Mesh).
     * General move/promote/transfer remain unavailable for this topology.
     */
    static bool CanGroupExternalParentSiblings(UOriginPivotComponent* Pivot);
    /** Read-only same-external-parent SCS roots, and socket-attached subset. */
    static void ListExternalParentSiblings(UOriginPivotComponent* Pivot,
        TArray<FName>& OutAll, TArray<FName>& OutSocketAttached);
    /** Safely create one authored Pivot under the same native/inherited parent,
     * group selected authored sibling roots, and preserve their common socket.
     * Never modify the native/inherited parent component.
     */
    static bool GroupExternalParentSiblings(UOriginPivotComponent* Pivot,
        const TArray<FName>& Variables, FName& OutNewPivotVariable, FText& OutReason);
    /** Adopt same-parent authored siblings into the selected existing Pivot,
     * rather than create another one. Only supported when the reference Pivot
     * has no authored children; it adopts the siblings' common socket and
     * moves to their average position with each branch compensated.
     */
    static bool AdoptExternalParentSiblings(UOriginPivotComponent* Pivot,
        const TArray<FName>& Variables, FText& OutReason);
    /** Read-only gate for offering the existing-Pivot action in Details. */
    static bool CanAdoptExternalParentSiblings(UOriginPivotComponent* Pivot);
    /** The pivot and sibling must have the same authored SCS parent. */
    static bool AdoptSibling(UOriginPivotComponent* Pivot, FName SiblingVariable, FText& OutReason);
    /** Move one authored direct child of the pivot up to the pivot's SCS parent. */
    static bool PromoteChild(UOriginPivotComponent* Pivot, FName ChildVariable, FText& OutReason);
    /** Insert a new Origin Pivot immediately above one authored direct child. */
    static bool InsertPivotAboveChild(UOriginPivotComponent* Pivot, FName ChildVariable,
        FName& OutNewPivotVariable, FText& OutReason);
    /** Atomic batch restructuring of authored direct siblings. One transaction, one Blueprint refresh. */
    static bool AdoptSiblings(UOriginPivotComponent* Pivot, const TArray<FName>& Variables, FText& OutReason);
    /** Atomic batch promotion from the pivot to its immediate SCS parent. */
    static bool PromoteChildren(UOriginPivotComponent* Pivot, const TArray<FName>& Variables, FText& OutReason);
    /** Detach children from the pivot to the authored Blueprint root (never to a native/inherited root). */
    static bool DetachChildrenToRoot(UOriginPivotComponent* Pivot, const TArray<FName>& Variables, FText& OutReason);
    /** Insert ONE shared pivot above direct children, retaining placement and
     * their common socket attachment (or no socket). Mixed sockets are rejected.
     */
    static bool InsertSharedPivotAboveChildren(UOriginPivotComponent* Pivot, const TArray<FName>& Variables,
        FName& OutNewPivotVariable, FText& OutReason);
    /** Transfer direct authored children between any two compatible Origin Pivots in
     * the same entirely authored SCS root. Reject cyclic paths, sockets, inherited
     * frames, unsafe scales, and mixed-invalid batches without mutating the asset.
     */
    static bool TransferChildrenToPivot(UOriginPivotComponent* SourcePivot, FName TargetPivotVariable,
        const TArray<FName>& Variables, FText& OutReason);
    /** Targets safe for the current selected child branches. Read-only and selection-aware.
     * An empty selection returns all compatible authored pivots as a discovery aid.
     */
    static void ListTransferTargetsForChildren(UOriginPivotComponent* SourcePivot,
        const TArray<FName>& SelectedVariables, TArray<FName>& OutTargets);
    /** Compatible authored target pivots; actual selections receive full preflight. */
    static void ListTransferTargets(UOriginPivotComponent* SourcePivot, TArray<FName>& OutTargets);
    /** Group authored direct SIBLINGS of this Pivot beneath one newly created
     * Pivot attached to their unchanged common parent/socket. Useful for
     * multiple components on the same socket of an authored Skeletal Mesh.
     * Different sockets or mixing a socket with none is rejected atomically.
     */
    static bool InsertSharedPivotAboveSiblings(UOriginPivotComponent* Pivot, const TArray<FName>& Variables,
        FName& OutNewPivotVariable, FText& OutReason);
    /** Siblings eligible for grouping under a new socket-preserving Pivot. */
    static void ListInsertableSiblings(UOriginPivotComponent* Pivot, TArray<FName>& OutSiblings);
    /** Direct authored children eligible for socket-preserving Pivot insertion.
     * Unlike Promote/Transfer selections, insertion can retain a child's SCS
     * socket on the new Pivot (and clear it on that child).
     */
    static void ListInsertableChildren(UOriginPivotComponent* Pivot, TArray<FName>& OutChildren);
    /** Read-only options; never modify the asset or its preview. */
    static void ListSiblingsAndChildren(UOriginPivotComponent* Pivot,
        TArray<FName>& OutSiblings, TArray<FName>& OutChildren);
};
