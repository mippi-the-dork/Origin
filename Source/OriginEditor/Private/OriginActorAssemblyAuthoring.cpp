// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginActorAssemblyAuthoring.h"

#include "OriginAnchor.h"
#include "OriginOperations.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor/GroupActor.h"
#include "LevelUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "OriginActorAssembly"
DEFINE_LOG_CATEGORY_STATIC(LogOriginActorAssembly, Log, All);
namespace
{
    constexpr double TransformTolerance = 1.e-3;

    struct FActorSnapshot
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<USceneComponent> PreviousParent;
        FName PreviousSocket = NAME_None;
        FTransform WorldTransform = FTransform::Identity;
    };

    // The snapshots are root-first so rebuilding their transforms cannot displace
    // an already restored descendant. Root parent/socket are also saved for rollback.
    static void SnapshotBranch(AActor* Root, TArray<FActorSnapshot>& Snapshots)
    {
        TArray<AActor*> Branch;
        Origin::GatherBranch(Root, Branch);
        for (AActor* Actor : Branch)
        {
            USceneComponent* Component = Actor->GetRootComponent();
            FActorSnapshot& Snapshot = Snapshots.AddDefaulted_GetRef();
            Snapshot.Actor = Actor;
            Snapshot.PreviousParent = Component ? Component->GetAttachParent() : nullptr;
            Snapshot.PreviousSocket = Actor->GetAttachParentSocketName();
            Snapshot.WorldTransform = Actor->GetActorTransform();
        }
    }

    static bool IsUniformPositiveScale(const FVector& Scale)
    {
        return Scale.X > KINDA_SMALL_NUMBER && Scale.Y > KINDA_SMALL_NUMBER &&
            Scale.Z > KINDA_SMALL_NUMBER && FMath::IsNearlyEqual(Scale.X, Scale.Y, 1.e-4) &&
            FMath::IsNearlyEqual(Scale.Y, Scale.Z, 1.e-4);
    }

    static bool IsUsable(AActor* Actor, UWorld* World, ULevel* Level, FText& Reason)
    {
        if (!Origin::IsEditorActor(Actor) || !Actor->GetRootComponent())
        {
            Reason = LOCTEXT("Loaded", "Every affected Actor must be a loaded, editable Level Editor Actor with a scene root.");
            return false;
        }
        if (Actor->GetWorld() != World || Actor->GetLevel() != Level)
        {
            Reason = LOCTEXT("SameLevel", "All affected Actors must belong to the same loaded level and editor world.");
            return false;
        }
        if (FLevelUtils::IsLevelLocked(Actor->GetLevel()) || Origin::IsProtected(Actor) || !Actor->IsEditable())
        {
            Reason = FText::Format(LOCTEXT("Locked", "Actor '{0}' is locked or not editable."), FText::FromString(Actor->GetActorLabel()));
            return false;
        }
        if (Actor->IsChildActor() || Actor->IsA<AGroupActor>() || Actor->GroupActor != nullptr)
        {
            Reason = LOCTEXT("GeneratedOrGrouped", "Child Actor Component instances and grouped Actors cannot be restructured. Edit the owning Blueprint or ungroup first.");
            return false;
        }
        if (Actor->GetRootComponent()->IsSimulatingPhysics())
        {
            Reason = LOCTEXT("Physics", "Disable root-component physics simulation before changing this attachment hierarchy.");
            return false;
        }
        if (!IsUniformPositiveScale(Actor->GetActorScale3D()))
        {
            Reason = LOCTEXT("Scale", "Alpha 5.2 requires positive uniform Actor scale for the affected hierarchy. Non-uniform or negative scales can introduce attachment shear.");
            return false;
        }
        return true;
    }

    static bool BeginPreflight(const TArray<AActor*>& Roots, AActor* Target, FText& Reason)
    {
        if (!GEditor || GEditor->PlayWorld)
        {
            Reason = LOCTEXT("PIE", "Actor hierarchy authoring is only available in the Level Editor outside PIE and simulation.");
            return false;
        }
        if (Roots.IsEmpty() || !Origin::IsEditorActor(Roots[0]))
        {
            Reason = LOCTEXT("NoSelection", "Select one or more loaded editor Actors.");
            return false;
        }
        UWorld* World = Roots[0]->GetWorld();
        ULevel* Level = Roots[0]->GetLevel();
        // Until unloaded Actor descriptors and Data Layers receive dedicated
        // validation, never modify World Partition/OFPA actor attachment bundles.
        if (World->GetWorldPartition())
        {
            Reason = LOCTEXT("WP", "Alpha 5.2 does not restructure World Partition Actor attachments. Use a non-partitioned level for now.");
            return false;
        }
        TArray<AActor*> Affected;
        for (AActor* Root : Roots)
        {
            if (!Origin::IsEditorActor(Root) || Root->GetWorld() != World || Root->GetLevel() != Level)
            {
                Reason = LOCTEXT("CrossLevel", "All selected Actors must be in the same loaded level and editor world.");
                return false;
            }
            // Reject overlapping branches; this would otherwise produce selection-order-dependent results.
            for (AActor* Ancestor = Root->GetAttachParentActor(); Ancestor; Ancestor = Ancestor->GetAttachParentActor())
                if (Roots.Contains(Ancestor))
                {
                    Reason = LOCTEXT("NestedSelection", "Select independent attachment roots, not a parent and one of its descendants together.");
                    return false;
                }
            Origin::GatherBranch(Root, Affected);
        }
        if (Target)
        {
            if (Target->GetWorld() != World || Target->GetLevel() != Level)
            {
                Reason = LOCTEXT("TargetLevel", "The attachment target must belong to the same editor level.");
                return false;
            }
            Affected.AddUnique(Target);
            for (AActor* Ancestor = Target->GetAttachParentActor(); Ancestor; Ancestor = Ancestor->GetAttachParentActor())
                Affected.AddUnique(Ancestor);
        }
        for (AActor* Root : Roots)
        {
            // Include old ancestors because attachment propagation/Undo can touch them.
            for (AActor* Ancestor = Root->GetAttachParentActor(); Ancestor; Ancestor = Ancestor->GetAttachParentActor())
                Affected.AddUnique(Ancestor);
        }
        for (AActor* Actor : Affected)
            if (!IsUsable(Actor, World, Level, Reason)) return false;
        // An Actor's root may be attached to a non-root SceneComponent; an
        // otherwise uniform Actor chain can still inherit non-uniform component scale.
        for (AActor* Root : Roots)
        {
            USceneComponent* ParentComponent = Root->GetRootComponent()->GetAttachParent();
            if (ParentComponent && !IsUniformPositiveScale(ParentComponent->GetComponentTransform().GetScale3D()))
            {
                Reason = LOCTEXT("ParentComponentScale", "An attachment parent SceneComponent has non-uniform or negative world scale. Restructuring this branch could introduce shear.");
                return false;
            }
        }
        return true;
    }

    static void ModifyForAttachment(const TArray<FActorSnapshot>& Snapshots, AActor* Target)
    {
        auto Modify = [](AActor* Actor)
        {
            if (!IsValid(Actor)) return;
            Actor->Modify();
            if (USceneComponent* Root = Actor->GetRootComponent()) Root->Modify();
            Actor->MarkPackageDirty();
        };
        for (const FActorSnapshot& Snapshot : Snapshots)
        {
            Modify(Snapshot.Actor.Get());
            if (USceneComponent* OldParent = Snapshot.PreviousParent.Get()) Modify(OldParent->GetOwner());
        }
        Modify(Target);
    }

    static bool AttachTo(AActor* Actor, USceneComponent* Parent, FName Socket)
    {
        if (!IsValid(Actor) || !Actor->GetRootComponent()) return false;
        if (Parent)
            return Actor->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform, Socket);
        Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        return Actor->GetRootComponent()->GetAttachParent() == nullptr;
    }

    static bool RestoreWorld(const TArray<FActorSnapshot>& Snapshots)
    {
        for (const FActorSnapshot& Snapshot : Snapshots)
            if (AActor* Actor = Snapshot.Actor.Get())
                Actor->SetActorTransform(Snapshot.WorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
        for (const FActorSnapshot& Snapshot : Snapshots)
        {
            AActor* Actor = Snapshot.Actor.Get();
            if (!Actor || !Actor->GetActorTransform().Equals(Snapshot.WorldTransform, TransformTolerance)) return false;
        }
        return true;
    }

    static bool VerifyPlacement(const TArray<FActorSnapshot>& Snapshots, FText& Reason)
    {
        for (const FActorSnapshot& Snapshot : Snapshots)
        {
            AActor* Actor = Snapshot.Actor.Get();
            if (!Actor || !Actor->GetActorTransform().Equals(Snapshot.WorldTransform, TransformTolerance))
            {
                Reason = FText::Format(LOCTEXT("Displaced", "Actor '{0}' changed world transform during attachment. Operation rejected."),
                    FText::FromString(Actor ? Actor->GetActorLabel() : TEXT("Missing Actor")));
                return false;
            }
        }
        return true;
    }

    static bool VerifyParent(AActor* Actor, USceneComponent* ExpectedParent, FName Socket, FText& Reason)
    {
        USceneComponent* Root = Actor ? Actor->GetRootComponent() : nullptr;
        if (!Root || Root->GetAttachParent() != ExpectedParent ||
            (ExpectedParent && Root->GetAttachSocketName() != Socket))
        {
            Reason = FText::Format(LOCTEXT("WrongParent", "Actor '{0}' has an unexpected attachment parent or socket. Operation rejected."),
                FText::FromString(Actor ? Actor->GetActorLabel() : TEXT("Missing Actor")));
            return false;
        }
        return true;
    }

    static bool VerifyDescendantLinks(const TArray<FActorSnapshot>& Snapshots,
        const TArray<AActor*>& ChangedRoots, FText& Reason)
    {
        for (const FActorSnapshot& Snapshot : Snapshots)
        {
            AActor* Actor = Snapshot.Actor.Get();
            if (ChangedRoots.Contains(Actor)) continue;
            USceneComponent* Root = Actor ? Actor->GetRootComponent() : nullptr;
            if (!Root || Root->GetAttachParent() != Snapshot.PreviousParent.Get() ||
                (Snapshot.PreviousParent.IsValid() && Root->GetAttachSocketName() != Snapshot.PreviousSocket))
            {
                Reason = FText::Format(LOCTEXT("DescendantChanged", "Descendant '{0}' changed its attachment unexpectedly. Operation rejected."),
                    FText::FromString(Actor ? Actor->GetActorLabel() : TEXT("Missing Actor")));
                return false;
            }
        }
        return true;
    }

    static bool RestorePreviousParents(const TArray<AActor*>& Roots, const TArray<FActorSnapshot>& Snapshots)
    {
        bool bGood = true;
        for (AActor* Root : Roots)
        {
            const FActorSnapshot* Snapshot = Snapshots.FindByPredicate([Root](const FActorSnapshot& S) { return S.Actor.Get() == Root; });
            if (!Snapshot || !AttachTo(Root, Snapshot->PreviousParent.Get(), Snapshot->PreviousSocket)) bGood = false;
        }
        // If Unreal disturbed a descendant's link as an indirect side effect,
        // restore that too rather than only restoring the explicitly moved roots.
        for (const FActorSnapshot& Snapshot : Snapshots)
        {
            AActor* Actor = Snapshot.Actor.Get();
            if (!Actor || Roots.Contains(Actor)) continue;
            USceneComponent* Root = Actor->GetRootComponent();
            if (!Root || Root->GetAttachParent() != Snapshot.PreviousParent.Get() ||
                (Snapshot.PreviousParent.IsValid() && Root->GetAttachSocketName() != Snapshot.PreviousSocket))
            {
                if (!AttachTo(Actor, Snapshot.PreviousParent.Get(), Snapshot.PreviousSocket)) bGood = false;
            }
        }
        if (!RestoreWorld(Snapshots)) bGood = false;
        for (const FActorSnapshot& Snapshot : Snapshots)
        {
            AActor* Actor = Snapshot.Actor.Get();
            USceneComponent* Root = Actor ? Actor->GetRootComponent() : nullptr;
            if (!Root || Root->GetAttachParent() != Snapshot.PreviousParent.Get() ||
                (Snapshot.PreviousParent.IsValid() && Root->GetAttachSocketName() != Snapshot.PreviousSocket))
                bGood = false;
        }
        return bGood;
    }

    static void AfterSuccess(UWorld* World)
    {
        if (GEditor && World == GEditor->GetEditorWorldContext().World())
        {
            GEditor->NoteSelectionChange();
            GEditor->RedrawLevelEditingViewports();
        }
    }

    static bool HasCompatibleMobility(const USceneComponent* Child,
        const USceneComponent* Destination, FText& Reason)
    {
        // Match Unreal attachment mobility before opening a transaction.
        // In particular Static children cannot attach beneath Movable parents.
        if (Child && Destination &&
            static_cast<uint8>(Child->Mobility) < static_cast<uint8>(Destination->Mobility))
        {
            Reason = LOCTEXT("AttachmentMobility", "This attachment would put a Static or Stationary Actor beneath a more mobile parent. Change component mobility before restructuring.");
            return false;
        }
        return true;
    }

    static bool CanBeAttachedTo(AActor* Parent, AActor* Child, FText& Reason)
    {
        // The Level Editor's native parenting policy handles mobility, sockets,
        // actor restrictions, and engine-defined attachability. Isolated test
        // worlds cannot be passed to the main Level Editor's selection policy.
        if (GEditor && Child->GetWorld() == GEditor->GetEditorWorldContext().World() &&
            Parent && !GEditor->CanParentActors(Parent, Child, &Reason)) return false;
        return true;
    }

    static bool ExecuteMove(const TArray<AActor*>& Roots, USceneComponent* ParentComponent,
        FName ParentSocket, AActor* Target, const FText& Label, FText& Reason)
    {
        TArray<FActorSnapshot> Snapshots;
        for (AActor* Root : Roots) SnapshotBranch(Root, Snapshots);
        FScopedTransaction Transaction(Label);
        ModifyForAttachment(Snapshots, Target);
        for (AActor* Root : Roots)
        {
            if (!AttachTo(Root, ParentComponent, ParentSocket) ||
                !RestoreWorld(Snapshots) ||
                !VerifyParent(Root, ParentComponent, ParentSocket, Reason) ||
                !VerifyPlacement(Snapshots, Reason) ||
                !VerifyDescendantLinks(Snapshots, Roots, Reason))
            {
                if (Reason.IsEmpty()) Reason = LOCTEXT("AttachFailed", "Unreal rejected an attachment. The original hierarchy is being restored.");
                const bool bRestored = RestorePreviousParents(Roots, Snapshots);
                Transaction.Cancel();
                if (!bRestored)
                    Reason = FText::Format(LOCTEXT("RollbackFailed", "{0} Rollback could not verify the original hierarchy. Undo and do not save this level."), Reason);
                UE_LOG(LogOriginActorAssembly, Warning, TEXT("%s"), *Reason.ToString());
                return false;
            }
        }
        // The entire branch, not just its root, must still occupy the exact initial world transform.
        if (!VerifyPlacement(Snapshots, Reason) || !VerifyDescendantLinks(Snapshots, Roots, Reason))
        {
            const bool bRestored = RestorePreviousParents(Roots, Snapshots);
            Transaction.Cancel();
            if (!bRestored) Reason = FText::Format(LOCTEXT("RollbackFinal", "{0} Rollback was incomplete; do not save."), Reason);
            return false;
        }
        AfterSuccess(Roots[0]->GetWorld());
        return true;
    }
}

namespace OriginActorAssembly
{
    bool CanAdopt(AOriginAnchor* Target, const TArray<AActor*>& Actors, FText& Reason)
    {
        if (!Origin::IsEditorActor(Target))
        {
            Reason = LOCTEXT("TargetAnchor", "Select one Origin Anchor and one or more other Actors in the World Outliner.");
            return false;
        }
        if (!BeginPreflight(Actors, Target, Reason)) return false;
        if (!IsUniformPositiveScale(Target->GetRootComponent()->GetComponentTransform().GetScale3D()))
        {
            Reason = LOCTEXT("AdoptTargetScale", "The target Anchor's root has non-uniform or negative world scale.");
            return false;
        }
        for (AActor* Actor : Actors)
        {
            if (Actor == Target)
            {
                Reason = LOCTEXT("Self", "An Origin Anchor cannot be its own child.");
                return false;
            }
            for (AActor* Ancestor = Target; Ancestor; Ancestor = Ancestor->GetAttachParentActor())
                if (Actor == Ancestor)
                {
                    Reason = LOCTEXT("Cycle", "Cannot attach an ancestor beneath its descendant.");
                    return false;
                }
            if (Actor->GetRootComponent()->GetAttachParent() == Target->GetRootComponent())
            {
                Reason = LOCTEXT("AlreadyAdopted", "An Actor is already directly attached to that Origin Anchor.");
                return false;
            }
            if (!HasCompatibleMobility(Actor->GetRootComponent(), Target->GetRootComponent(), Reason) ||
                !CanBeAttachedTo(Target, Actor, Reason)) return false;
        }
        return true;
    }

    bool Adopt(AOriginAnchor* Target, const TArray<AActor*>& Actors, FText& Reason)
    {
        if (!CanAdopt(Target, Actors, Reason)) return false;
        return ExecuteMove(Actors, Target->GetRootComponent(), NAME_None, Target,
            LOCTEXT("AdoptTransaction", "Origin: Reparent Actors to Anchor"), Reason);
    }

    bool CanInsert(AActor* Child, FText& Reason)
    {
        if (!Origin::IsEditorActor(Child))
        {
            Reason = LOCTEXT("InsertSelection", "Select one Actor in the Level Editor to insert an Origin Anchor above it.");
            return false;
        }
        if (!BeginPreflight({Child}, Child->GetAttachParentActor(), Reason)) return false;
        // Inserting a root-level anchor is allowed. Non-uniform parent transforms
        // have already been rejected by BeginPreflight.
        return true;
    }

    bool Insert(AActor* Child, AOriginAnchor*& NewAnchor, FText& Reason)
    {
        NewAnchor = nullptr;
        if (!CanInsert(Child, Reason)) return false;
        UWorld* World = Child->GetWorld();
        ULevel* Level = Child->GetLevel();
        USceneComponent* PreviousParent = Child->GetRootComponent()->GetAttachParent();
        const FName PreviousSocket = Child->GetAttachParentSocketName();
        TArray<FActorSnapshot> Snapshots;
        SnapshotBranch(Child, Snapshots);
        const FTransform AnchorWorld(Child->GetActorQuat(), Child->GetActorLocation(), FVector::OneVector);

        FScopedTransaction Transaction(LOCTEXT("InsertTransaction", "Origin: Insert Anchor Above Actor"));
        // Spawning changes the level's Actor array as well as the new Actor.
        // Record the level before AddActor/SpawnActor so Undo can remove or
        // restore the newly introduced assembly node as one operation.
        Level->Modify();
        AOriginAnchor* Anchor = nullptr;
        if (World == GEditor->GetEditorWorldContext().World())
            Anchor = Cast<AOriginAnchor>(GEditor->AddActor(Level, AOriginAnchor::StaticClass(), AnchorWorld, true, RF_Transactional, false));
        else
        {
            // Isolated native automation worlds use the same actual hierarchy
            // algorithm, but cannot route actor spawning through the active map editor.
            FActorSpawnParameters Params;
            Params.OverrideLevel = Level;
            Params.ObjectFlags |= RF_Transactional;
            Anchor = World->SpawnActor<AOriginAnchor>(AOriginAnchor::StaticClass(), AnchorWorld, Params);
        }
        if (!Anchor)
        {
            Reason = LOCTEXT("SpawnFailed", "Unreal could not spawn an Origin Anchor in the Actor's level.");
            Transaction.Cancel();
            return false;
        }
        Anchor->SetActorLabel(TEXT("Origin Anchor"));
        Anchor->SetFolderPath(Child->GetFolderPath());
        // Match the inserted Anchor to the existing child's root mobility.
        // Unreal does not allow a Static intermediate component to attach to a
        // Movable (or Stationary) parent. The original child is already attached
        // to that parent, so its mobility is a compatible choice for both links.
        // Keep this per-instance: changing AOriginAnchor's native default would
        // silently alter existing levels and unrelated Create Anchor behavior.
        Anchor->GetRootComponent()->SetMobility(Child->GetRootComponent()->Mobility);
        ModifyForAttachment(Snapshots, Child->GetAttachParentActor());
        Anchor->Modify();
        Anchor->GetRootComponent()->Modify();
        bool bAttached = CanBeAttachedTo(Child->GetAttachParentActor(), Anchor, Reason) &&
            CanBeAttachedTo(Anchor, Child, Reason);
        if (bAttached) bAttached = AttachTo(Anchor, PreviousParent, PreviousSocket);
        if (bAttached) bAttached = AttachTo(Child, Anchor->GetRootComponent(), NAME_None);
        if (bAttached) bAttached = RestoreWorld(Snapshots);
        if (bAttached) bAttached = VerifyParent(Anchor, PreviousParent, PreviousSocket, Reason);
        if (bAttached) bAttached = VerifyParent(Child, Anchor->GetRootComponent(), NAME_None, Reason);
        if (bAttached) bAttached = VerifyPlacement(Snapshots, Reason);
        if (bAttached) bAttached = VerifyDescendantLinks(Snapshots, {Child}, Reason);
        if (!bAttached)
        {
            if (Reason.IsEmpty()) Reason = LOCTEXT("InsertAttachFailure", "Unreal rejected pivot insertion. Restoring the previous attachment.");
            const bool bRestored = RestorePreviousParents({Child}, Snapshots);
            World->EditorDestroyActor(Anchor, true);
            Transaction.Cancel();
            if (!bRestored) Reason = FText::Format(LOCTEXT("InsertRollbackFailure", "{0} Rollback was incomplete; do not save this level."), Reason);
            return false;
        }
        Anchor->CustomPivot = Anchor->GetActorLocation();
        Anchor->MarkPackageDirty();
        NewAnchor = Anchor;
        AfterSuccess(World);
        return true;
    }

    static bool CanMoveFromAnchor(const TArray<AActor*>& Children, bool bDetach, FText& Reason)
    {
        if (Children.IsEmpty() || !Origin::IsEditorActor(Children[0]))
        {
            Reason = LOCTEXT("PromoteSelection", "Select one or more direct children of the same Origin Anchor.");
            return false;
        }
        AOriginAnchor* Anchor = Cast<AOriginAnchor>(Children[0]->GetAttachParentActor());
        if (!Anchor)
        {
            Reason = LOCTEXT("NotAnchorChild", "Selected Actors must be directly attached to an Origin Anchor.");
            return false;
        }
        if (!BeginPreflight(Children, Anchor, Reason)) return false;
        for (AActor* Child : Children)
        {
            if (Child->GetAttachParentActor() != Anchor || Child->GetRootComponent()->GetAttachParent() != Anchor->GetRootComponent())
            {
                Reason = LOCTEXT("DifferentParents", "Selected Actors must be direct root-component children of the same Origin Anchor.");
                return false;
            }
        }
        USceneComponent* Destination = bDetach ? nullptr : Anchor->GetRootComponent()->GetAttachParent();
        if (Destination && !IsUniformPositiveScale(Destination->GetComponentTransform().GetScale3D()))
        {
            Reason = LOCTEXT("PromoteTargetScale", "The Anchor's parent SceneComponent has non-uniform or negative scale. Promotion is rejected to avoid changing the child's world transform.");
            return false;
        }
        AActor* Parent = Destination ? Destination->GetOwner() : nullptr;
        for (AActor* Child : Children)
            if (!HasCompatibleMobility(Child->GetRootComponent(), Destination, Reason) ||
                !CanBeAttachedTo(Parent, Child, Reason)) return false;
        return true;
    }

    bool CanPromote(const TArray<AActor*>& Children, FText& Reason)
    { return CanMoveFromAnchor(Children, false, Reason); }

    bool Promote(const TArray<AActor*>& Children, FText& Reason)
    {
        if (!CanPromote(Children, Reason)) return false;
        AOriginAnchor* Anchor = Cast<AOriginAnchor>(Children[0]->GetAttachParentActor());
        return ExecuteMove(Children, Anchor->GetRootComponent()->GetAttachParent(),
            Anchor->GetAttachParentSocketName(), Anchor,
            LOCTEXT("PromoteTransaction", "Origin: Promote Actors from Anchor"), Reason);
    }

    bool CanDetach(const TArray<AActor*>& Children, FText& Reason)
    { return CanMoveFromAnchor(Children, true, Reason); }

    bool Detach(const TArray<AActor*>& Children, FText& Reason)
    {
        if (!CanDetach(Children, Reason)) return false;
        AOriginAnchor* Anchor = Cast<AOriginAnchor>(Children[0]->GetAttachParentActor());
        return ExecuteMove(Children, nullptr, NAME_None, Anchor,
            LOCTEXT("DetachTransaction", "Origin: Detach Actors from Anchor"), Reason);
    }
}
#undef LOCTEXT_NAMESPACE
