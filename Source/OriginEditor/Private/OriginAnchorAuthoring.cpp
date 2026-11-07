// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginAnchorAuthoring.h"

#include "OriginAnchor.h"
#include "OriginOperations.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

namespace
{
    bool SameWorld(const FTransform& A, const FTransform& B)
    {
        return A.GetLocation().Equals(B.GetLocation(), 0.01) &&
            A.GetRotation().Equals(B.GetRotation(), 1.e-4) &&
            A.GetScale3D().Equals(B.GetScale3D(), 1.e-4);
    }

    bool UniformPositive(const FVector& Scale)
    {
        return Scale.X > KINDA_SMALL_NUMBER && Scale.Y > KINDA_SMALL_NUMBER &&
            Scale.Z > KINDA_SMALL_NUMBER &&
            FMath::IsNearlyEqual(Scale.X, Scale.Y, 1.e-4) &&
            FMath::IsNearlyEqual(Scale.Y, Scale.Z, 1.e-4);
    }

    struct FChildSnapshot
    {
        TWeakObjectPtr<USceneComponent> Component;
        FTransform World;
        TWeakObjectPtr<USceneComponent> Parent;
        FName Socket = NAME_None;
        bool bDirect = false;
    };

    bool CanEditStaticAnchor(AOriginAnchor* Anchor, const FTransform& Candidate, FText& Reason)
    {
        USceneComponent* Root = Anchor ? Anchor->GetRootComponent() : nullptr;
        if (!IsValid(Root) || !Root->IsRegistered() || Root->Mobility == EComponentMobility::Movable)
        {
            Reason = FText::FromString(TEXT("Static Anchor editor editing requires a registered Static/Stationary root."));
            return false;
        }
        const FTransform Current = Root->GetComponentTransform();
        if (!Candidate.IsValid() || Candidate.ContainsNaN() || !Candidate.GetRotation().IsNormalized() ||
            !Candidate.GetScale3D().Equals(Current.GetScale3D(), 1.e-4) ||
            !UniformPositive(Current.GetScale3D()))
        {
            Reason = FText::FromString(TEXT("Anchor slot activation requires a valid pose and unchanged, positive uniform scale."));
            return false;
        }
        for (const USceneComponent* Parent = Root->GetAttachParent(); Parent; Parent = Parent->GetAttachParent())
        {
            if (!Parent->IsRegistered() || !UniformPositive(Parent->GetComponentTransform().GetScale3D()))
            {
                Reason = FText::FromString(TEXT("An ancestor component is unregistered or has unsupported scale."));
                return false;
            }
            if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Parent))
                if (Primitive->IsSimulatingPhysics())
                {
                    Reason = FText::FromString(TEXT("An ancestor is simulating physics; disable simulation before moving the pivot."));
                    return false;
                }
        }
        TArray<USceneComponent*> All;
        Root->GetChildrenComponents(true, All);
        for (USceneComponent* Child : All)
        {
            if (!IsValid(Child) || Child->IsVisualizationComponent()) continue;
            if (!Child->IsRegistered() || Child->GetComponentTransform().ContainsNaN() ||
                !UniformPositive(Child->GetComponentTransform().GetScale3D()))
            {
                Reason = FText::FromString(TEXT("Every attached scene component must be registered with a positive uniform transform."));
                return false;
            }
            if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child))
                if (Primitive->IsSimulatingPhysics())
                {
                    Reason = FText::FromString(TEXT("Disable physics simulation before activating a Static Anchor slot."));
                    return false;
                }
        }
        return true;
    }

    bool ApplyStaticAnchorPose(AOriginAnchor* Anchor, const FTransform& Candidate, FText& Reason)
    {
        USceneComponent* Root = Anchor->GetRootComponent();
        const FTransform Before = Root->GetComponentTransform();
        TArray<USceneComponent*> All;
        Root->GetChildrenComponents(true, All);
        TArray<FChildSnapshot> Snapshots;
        for (USceneComponent* Child : All)
        {
            if (!IsValid(Child) || Child->IsVisualizationComponent()) continue;
            FChildSnapshot& Item = Snapshots.AddDefaulted_GetRef();
            Item.Component = Child;
            Item.World = Child->GetComponentTransform();
            Item.Parent = Child->GetAttachParent();
            Item.Socket = Child->GetAttachSocketName();
            Item.bDirect = Child->GetAttachParent() == Root;
        }
        Anchor->SetActorTransform(Candidate, false, nullptr, ETeleportType::TeleportPhysics);
        for (const FChildSnapshot& Item : Snapshots)
            if (Item.bDirect) if (USceneComponent* Child = Item.Component.Get())
                Child->SetWorldTransform(Item.World, false, nullptr, ETeleportType::TeleportPhysics);
        auto Verify = [&]()
        {
            for (const FChildSnapshot& Item : Snapshots)
            {
                USceneComponent* Child = Item.Component.Get();
                if (!Child || !SameWorld(Item.World, Child->GetComponentTransform()) ||
                    Child->GetAttachParent() != Item.Parent.Get() ||
                    Child->GetAttachSocketName() != Item.Socket) return false;
            }
            return true;
        };
        if (SameWorld(Candidate, Root->GetComponentTransform()) && Verify())
        { Reason = FText::GetEmpty(); return true; }

        // On any failure, restore the original root frame followed by direct
        // child frames; a failure must never silently displace level geometry.
        Anchor->SetActorTransform(Before, false, nullptr, ETeleportType::TeleportPhysics);
        for (const FChildSnapshot& Item : Snapshots)
            if (Item.bDirect) if (USceneComponent* Child = Item.Component.Get())
                Child->SetWorldTransform(Item.World, false, nullptr, ETeleportType::TeleportPhysics);
        const bool bRestored = SameWorld(Before, Root->GetComponentTransform()) && Verify();
        Reason = FText::FromString(bRestored
            ? TEXT("Unreal could not preserve the Actor attachment hierarchy; the Anchor pose was restored.")
            : TEXT("Anchor slot rollback could not be verified. Undo and do not save this level."));
        return false;
    }
}

bool FOriginAnchorAuthoring::CanAuthor(AOriginAnchor* Anchor, FText& Reason)
{
    if (!Origin::CanOperate(Anchor, Reason)) return false;
    if (!Anchor || !Anchor->GetRootComponent())
    {
        Reason = FText::FromString(TEXT("Origin Anchor needs a valid SceneComponent root."));
        return false;
    }
    return true;
}

bool FOriginAnchorAuthoring::RunSlotAction(AOriginAnchor* Anchor, EOriginAnchorSlotAction Action,
    FName Selected, FName NewName, FText& Reason)
{
    if (!CanAuthor(Anchor, Reason)) return false;
    USceneComponent* Root = Anchor->GetRootComponent();
    bool bUseStaticEditorMove = false;
    FTransform StaticCandidate = FTransform::Identity;
    // Resolve the exact runtime candidate before modifying Undo state.
    if (Action == EOriginAnchorSlotAction::Activate)
    {
        const FOriginPivotSlot* Slot = Anchor->OriginPivotConfiguration.FindSlot(Selected);
        if (!Slot || !Slot->bHasSavedRelativeTransform)
        {
            Reason = FText::FromString(TEXT("Capture the selected slot before activating it."));
            return false;
        }
        const USceneComponent* Parent = Root->GetAttachParent();
        if (Slot->bCapturedAtWorldRoot != (Parent == nullptr))
        {
            Reason = FText::FromString(TEXT("This slot was captured in another attachment frame. Recapture it first."));
            return false;
        }
        const FName Socket = Root->GetAttachSocketName();
        if (Slot->CapturedSocketName != Socket ||
            (Parent && !Socket.IsNone() && !Parent->DoesSocketExist(Socket)))
        {
            Reason = FText::FromString(TEXT("This slot was captured in another socket frame or its socket is missing. Recapture it first."));
            return false;
        }
        const FTransform Candidate = Parent
            ? Slot->SavedRelativeTransform * Parent->GetSocketTransform(Socket, RTS_World)
            : Slot->SavedRelativeTransform;
        const EOriginPivotResult Preflight = Anchor->CanSetOriginPivotWorldTransform(Candidate, Reason);
        if (Preflight == EOriginPivotResult::UnsupportedMobility &&
            Root->Mobility != EComponentMobility::Movable)
        {
            if (!CanEditStaticAnchor(Anchor, Candidate, Reason)) return false;
            bUseStaticEditorMove = true;
            StaticCandidate = Candidate;
        }
        else if (Preflight != EOriginPivotResult::Success && Preflight != EOriginPivotResult::NoChange)
            return false;
        if (Preflight == EOriginPivotResult::NoChange &&
            Anchor->OriginPivotConfiguration.ActiveSlotName == Selected)
        {
            Reason = FText::GetEmpty();
            return true; // No-op; do not fill Undo history.
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("Origin", "AnchorSlotEdit", "Origin: Edit Anchor Pivot Slot"));
    Anchor->Modify();
    Root->Modify();
    if (Action == EOriginAnchorSlotAction::Activate)
    {
        TArray<USceneComponent*> Children;
        Root->GetChildrenComponents(true, Children);
        TSet<AActor*> Owners;
        for (USceneComponent* Child : Children) if (IsValid(Child))
        {
            Child->Modify();
            if (AActor* Owner = Child->GetOwner()) Owners.Add(Owner);
        }
        for (AActor* Owner : Owners) if (IsValid(Owner)) Owner->Modify();
    }
    bool bOK = false;
    switch (Action)
    {
    case EOriginAnchorSlotAction::Create:
        bOK = Anchor->CreateOriginPivotSlot(NewName, Reason); break;
    case EOriginAnchorSlotAction::Capture:
        bOK = Anchor->CaptureOriginPivotSlot(Selected, Reason); break;
    case EOriginAnchorSlotAction::Activate:
    {
        if (bUseStaticEditorMove)
        {
            const FTransform Before = Anchor->GetActorTransform();
            bOK = ApplyStaticAnchorPose(Anchor, StaticCandidate, Reason);
            if (bOK)
            {
                Anchor->OriginPivotConfiguration.ActiveSlotName = Selected;
                if (!SameWorld(Before, Anchor->GetActorTransform()))
                    Anchor->OnOriginPivotChanged.Broadcast(Before, Anchor->GetActorTransform());
            }
        }
        else
        {
            const EOriginPivotResult Result = Anchor->ActivateOriginPivotSlot(Selected, Reason);
            bOK = Result == EOriginPivotResult::Success || Result == EOriginPivotResult::NoChange;
        }
        break;
    }
    case EOriginAnchorSlotAction::Duplicate:
        bOK = Anchor->DuplicateOriginPivotSlot(Selected, NewName, Reason); break;
    case EOriginAnchorSlotAction::Rename:
        bOK = Anchor->RenameOriginPivotSlot(Selected, NewName, Reason); break;
    case EOriginAnchorSlotAction::Delete:
        bOK = Anchor->DeleteOriginPivotSlot(Selected, Reason); break;
    }
    if (!bOK)
    {
        Transaction.Cancel();
        return false;
    }
    Anchor->MarkPackageDirty();
    if (GEditor && Anchor->GetWorld() == GEditor->GetEditorWorldContext().World())
        GEditor->RedrawLevelEditingViewports();
    Reason = FText::GetEmpty();
    return true;
}
