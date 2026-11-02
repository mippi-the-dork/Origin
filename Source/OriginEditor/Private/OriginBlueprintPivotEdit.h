// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class USCS_Node;
class UOriginPivotComponent;
class USceneComponent;
class FScopedTransaction;

/** Authoring-only template edits. Never runs against a Blueprint preview instance alone. */
namespace OriginBlueprintPivot
{
    struct FChildTransform
    {
        TWeakObjectPtr<USCS_Node> Node;
        TWeakObjectPtr<USceneComponent> Template;
        FTransform Before;
        FTransform After;
    };

    struct FPlan
    {
        FTransform BeforePivot;
        FTransform AfterPivot;
        TArray<FChildTransform> Children;
    };

    /** Returns only Blueprint-owned, directly authored SCS templates; inherited overrides are excluded for now. */
    bool Resolve(UOriginPivotComponent* Pivot, UBlueprint*& OutBlueprint, USCS_Node*& OutNode);
    /** Maps a Blueprint component template OR its editor preview component back to the actual authored SCS template. */
    bool ResolveSelection(UOriginPivotComponent* Selected, UOriginPivotComponent*& OutTemplate,
        UBlueprint*& OutBlueprint, USCS_Node*& OutNode);
    /** Resolve a generated component on the actual Blueprint editor preview Actor.
     * The transient SCS EditorComponentInstance weak pointer is not guaranteed to be populated.
     * Prefer the compiled component variable on the editor preview Actor; never guess by name.
     * Returns null if the active preview cannot be verified. */
    USceneComponent* FindPreviewComponent(UBlueprint* Blueprint, USCS_Node* Node);
    /** Compare authored and generated relative transforms without treating the preview Actor's
     * own world transform as an authored component offset. */
    bool IsUnmodifiedPreviewTransform(const FTransform& Authored, const FTransform& Preview);
    /** Pure transform compensation. Safe to exercise without constructing a Blueprint asset or SCS. */
    bool CompensateChild(const FTransform& BeforePivot, const FTransform& AfterPivot,
        const FTransform& BeforeChild, FTransform& OutAfterChild);
    bool Plan(UOriginPivotComponent* Pivot, const FTransform& NewRelative, FPlan& OutPlan, FText& OutReason);
    void Apply(UOriginPivotComponent* Pivot, const FPlan& Plan, bool bUseAfter);
}

/** One Blueprint gizmo drag = one transaction; can roll back even after several deltas. */
class FOriginBlueprintPivotEditSession final
{
public:
    explicit FOriginBlueprintPivotEditSession(UOriginPivotComponent* InTemplate, bool bInRecordUndo = true);
    ~FOriginBlueprintPivotEditSession();
    /** Start the transaction before Unreal's Blueprint viewport can alter the preview
     * or SCS templates. Does not apply a transform. */
    void PrepareForViewportDrag();
    /** Restore the authored children from the beginning of this edit, even if the
     * Blueprint viewport has already moved the preview or component templates. */
    bool NeedsReconciliation(const FTransform& PreviewPivotRelative) const;
    bool Apply(const FTransform& NewRelative, FText& OutReason);
    void Finish();
    void Cancel();
    bool HasChanged() const { return bChanged; }

private:
    void Begin(const OriginBlueprintPivot::FPlan& Plan);
    TWeakObjectPtr<UOriginPivotComponent> Template;
    TWeakObjectPtr<UBlueprint> Blueprint;
    TWeakObjectPtr<USCS_Node> Node;
    FTransform InitialPivot;
    TArray<OriginBlueprintPivot::FChildTransform> OriginalChildren;
    TUniquePtr<FScopedTransaction> Transaction;
    bool bRecordUndo = true;
    bool bChanged = false;
    bool bBegun = false;
};
