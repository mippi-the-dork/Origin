// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginAnchor.h"
#include "OriginPivotSlots.h"
#include "Components/SceneComponent.h"
#if WITH_EDITOR
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#endif

AOriginAnchor::AOriginAnchor()
{
    PrimaryActorTick.bCanEverTick = false;
    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
    Root->SetMobility(EComponentMobility::Static);
    SetRootComponent(Root);
    SetActorEnableCollision(false);
#if WITH_EDITOR
    EditorBillboard = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("OriginEditorBillboard"));
    if (EditorBillboard)
    {
        // Recreated from native defaults; do not save the component's transient
        // imported texture into a level. Only the per-instance checkbox is saved.
        EditorBillboard->SetFlags(RF_Transient);
        EditorBillboard->SetupAttachment(Root);
        EditorBillboard->SetIsVisualizationComponent(true);
        EditorBillboard->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        EditorBillboard->SetHiddenInGame(true);
        EditorBillboard->bIsScreenSizeScaled = true;
        // Match USceneComponent::CreateSpriteComponent's editor visualization
        // settings. Keep UBillboardComponent's native screen-size threshold.
        EditorBillboard->bUseInEditorScaling = true;
        EditorBillboard->OpacityMaskRefVal = 0.3f;
        EditorBillboard->SetRelativeScale3D(FVector(0.5f));
    }
#endif
    // No editor-only actor flag: the root and attachment hierarchy must survive cooking.
}


void AOriginAnchor::PostLoad()
{
    Super::PostLoad();
    // This only serializes a mirror of existing authoring values. Do not call
    // SetActorTransform, attach/detach, load soft references, or mark a map dirty.
    // Legacy maps also migrate when loaded by the editor during cooking.
    if (!IsTemplate() && (OriginPivotConfiguration.SchemaVersion < 1 ||
                          !OriginPivotConfiguration.FindSlot(TEXT("Default"))))
    {
#if WITH_EDITOR
        SyncLegacyPivotToRuntimeDefinition();
#else
        // Editor-only legacy values cannot exist in a stripped runtime build.
        // Content cooked with Origin 2.0 already has its migrated slot saved.
        // An already-cooked 1.x package could have lost its editor-only
        // source configuration. Warn rather than imply the empty fallback
        // reconstructs those unavailable legacy settings.
        UE_LOG(LogTemp, Warning, TEXT("Origin: Anchor '%s' loaded without a runtime pivot definition. Re-cook the source level using Origin 2.0 to migrate legacy pivot settings."), *GetPathName());
        if (!OriginPivotConfiguration.FindSlot(TEXT("Default")))
        {
            OriginPivotConfiguration.Slots.AddDefaulted();
        }
        OriginPivotConfiguration.SchemaVersion = 1;
#endif
    }
}

void AOriginAnchor::PostActorCreated()
{
    Super::PostActorCreated();
    if (!IsTemplate() && OriginPivotConfiguration.SchemaVersion < 1)
    {
#if WITH_EDITOR
        SyncLegacyPivotToRuntimeDefinition();
#else
        if (!OriginPivotConfiguration.FindSlot(TEXT("Default")))
        {
            OriginPivotConfiguration.Slots.AddDefaulted();
        }
        OriginPivotConfiguration.SchemaVersion = 1;
#endif
    }
}

bool AOriginAnchor::GetOriginPivotSlotDefinition(FName Name, FOriginPivotDefinition& Out) const
{
    return FOriginPivotSlots::Definition(OriginPivotConfiguration, Name, Out);
}

namespace
{
    bool AnchorPose(const AOriginAnchor* Anchor, FTransform& OutPose, bool& bWorldFrame)
    {
        const USceneComponent* Root = Anchor ? Anchor->GetRootComponent() : nullptr;
        if (!IsValid(Root) || Root->IsTemplate()) return false;
        bWorldFrame = Root->GetAttachParent() == nullptr;
        OutPose = bWorldFrame ? Root->GetComponentTransform() : Root->GetRelativeTransform();
        return OutPose.IsValid() && !OutPose.ContainsNaN();
    }
}

bool AOriginAnchor::CreateOriginPivotSlot(FName Name, FText& Reason)
{
    FTransform Pose; bool bWorldFrame = false;
    if (!AnchorPose(this, Pose, bWorldFrame))
    { Reason = FText::FromString(TEXT("Origin Anchor requires a live root with a valid transform.")); return false; }
    return FOriginPivotSlots::Create(OriginPivotConfiguration, Name, Pose, bWorldFrame,
        GetRootComponent()->GetAttachSocketName(), Reason);
}

bool AOriginAnchor::CaptureOriginPivotSlot(FName Name, FText& Reason)
{
    FTransform Pose; bool bWorldFrame = false;
    if (!AnchorPose(this, Pose, bWorldFrame))
    { Reason = FText::FromString(TEXT("Origin Anchor requires a live root with a valid transform.")); return false; }
    return FOriginPivotSlots::Capture(OriginPivotConfiguration, Name, Pose, bWorldFrame,
        GetRootComponent()->GetAttachSocketName(), Reason);
}

bool AOriginAnchor::DuplicateOriginPivotSlot(FName Source, FName Name, FText& Reason)
{
    return FOriginPivotSlots::Duplicate(OriginPivotConfiguration, Source, Name, Reason);
}

bool AOriginAnchor::RenameOriginPivotSlot(FName OldName, FName NewName, FText& Reason)
{
    return FOriginPivotSlots::Rename(OriginPivotConfiguration, OldName, NewName, Reason);
}

void AOriginAnchor::GetOriginPivotSlotNames(TArray<FName>& Out) const
{
    FOriginPivotSlots::Names(OriginPivotConfiguration, Out);
}

bool AOriginAnchor::DeleteOriginPivotSlot(FName Name, FText& Reason)
{
    return FOriginPivotSlots::Delete(OriginPivotConfiguration, Name, Reason);
}

bool AOriginAnchor::GetOriginPivotSlotPose(FName Name, FTransform& Out) const
{
    return FOriginPivotSlots::Pose(OriginPivotConfiguration, Name, Out);
}

EOriginPivotResult AOriginAnchor::CanSetOriginPivotWorldTransform(const FTransform& Candidate, FText& Reason) const
{
    return FOriginPivotCore::ValidatePivotEdit(GetRootComponent(), Candidate, Reason);
}

EOriginPivotResult AOriginAnchor::SetOriginPivotWorldTransform(const FTransform& Candidate, FText& Reason)
{
    USceneComponent* Root = GetRootComponent();
    if (!Root)
    { Reason = FText::FromString(TEXT("Origin Anchor has no root component.")); return EOriginPivotResult::InvalidPivot; }
    const FTransform Before = Root->GetComponentTransform();
    const EOriginPivotResult Result = FOriginPivotCore::SetPivotWorldTransformPreservingChildren(Root, Candidate, Reason);
    if (Result == EOriginPivotResult::Success)
        OnOriginPivotChanged.Broadcast(Before, Root->GetComponentTransform());
    return Result;
}

EOriginPivotResult AOriginAnchor::ActivateOriginPivotSlot(FName Name, FText& Reason)
{
    FTransform Pose; bool bWasWorld = false;
    if (!FOriginPivotSlots::Pose(OriginPivotConfiguration, Name, Pose, &bWasWorld))
    { Reason = FText::FromString(TEXT("This pivot slot is missing or has never captured a transform.")); return EOriginPivotResult::InvalidTransform; }
    if (IsTemplate())
    { Reason = FText::FromString(TEXT("Pivot slots cannot be activated on Actor templates.")); return EOriginPivotResult::InvalidPivot; }
    const USceneComponent* Root = GetRootComponent();
    if (!Root)
    { Reason = FText::FromString(TEXT("Origin Anchor has no root component.")); return EOriginPivotResult::InvalidPivot; }
    const USceneComponent* Parent = Root->GetAttachParent();
    if (bWasWorld != (Parent == nullptr))
    {
        Reason = FText::FromString(TEXT("The Anchor attachment frame changed since capture. Recapture the slot in the current parent frame."));
        return EOriginPivotResult::InvalidPivot;
    }
    const FName Socket = Root->GetAttachSocketName();
    const FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(Name);
    if (!Slot || Slot->CapturedSocketName != Socket ||
        (Parent && !Socket.IsNone() && !Parent->DoesSocketExist(Socket)))
    {
        Reason = FText::FromString(TEXT("The Anchor attachment socket changed or is missing. Recapture the slot in the current attachment frame."));
        return EOriginPivotResult::InvalidPivot;
    }
    const FTransform Candidate = Parent ? Pose * Parent->GetSocketTransform(Socket, RTS_World) : Pose;
    const EOriginPivotResult Preflight = CanSetOriginPivotWorldTransform(Candidate, Reason);
    if (Preflight != EOriginPivotResult::Success && Preflight != EOriginPivotResult::NoChange) return Preflight;
    const EOriginPivotResult Result = Preflight == EOriginPivotResult::NoChange
        ? Preflight : SetOriginPivotWorldTransform(Candidate, Reason);
    if (Result == EOriginPivotResult::Success || Result == EOriginPivotResult::NoChange)
    {
        OriginPivotConfiguration.ActiveSlotName = Name;
        Reason = FText::GetEmpty();
    }
    return Result;
}

#if WITH_EDITOR
void AOriginAnchor::SyncLegacyPivotToRuntimeDefinition()
{
    FOriginPivotSlot* DefaultSlot = OriginPivotConfiguration.FindSlot(TEXT("Default"));
    if (!DefaultSlot)
    {
        FOriginPivotSlot NewSlot;
        NewSlot.Name = TEXT("Default");
        const int32 NewIndex = OriginPivotConfiguration.Slots.Add(MoveTemp(NewSlot));
        DefaultSlot = &OriginPivotConfiguration.Slots[NewIndex];
    }
#if WITH_EDITORONLY_DATA
    // Keep the original 1.x reflected property names/types as the stable
    // serialized input. Soft actor references are COPIED, never resolved.
    DefaultSlot->Definition.PositionMode = PivotMode;
    DefaultSlot->Definition.ChildActor = PivotChild;
    DefaultSlot->Definition.CustomPosition = CustomPivot;
    DefaultSlot->Definition.BoundsAxes = BoundsAxes;
    DefaultSlot->Definition.ReferenceActor = ReferenceActor;
    DefaultSlot->Definition.ComponentName = PivotComponentName;
    DefaultSlot->Definition.SocketName = PivotSocketName;
    DefaultSlot->Definition.Offset = OriginPivotOffset;
    DefaultSlot->Definition.bLocalOffset = bLocalPivotOffset;
#endif
    OriginPivotConfiguration.SchemaVersion = FMath::Max(1, OriginPivotConfiguration.SchemaVersion);
    if (OriginPivotConfiguration.ActiveSlotName.IsNone())
    {
        OriginPivotConfiguration.ActiveSlotName = TEXT("Default");
    }
}
#endif

#if WITH_EDITOR
void AOriginAnchor::RefreshEditorBillboard()
{
    if (!EditorBillboard || IsTemplate() || !GetWorld() ||
        GetWorld()->WorldType != EWorldType::Editor || IsRunningCommandlet()) return;
    // Reapply on registration/Undo so existing anchors also adopt the updated
    // visualization settings, without changing their root or child transforms.
    EditorBillboard->bIsScreenSizeScaled = true;
    EditorBillboard->bUseInEditorScaling = true;
    EditorBillboard->ScreenSize = GetDefault<UBillboardComponent>()->ScreenSize;
    EditorBillboard->OpacityMaskRefVal = 0.3f;
    EditorBillboard->SetRelativeScale3D(FVector(0.5f));
    // Import outside the constructor/CDO path. Existing billboard components
    // hold the texture alive; the weak cache never roots it indefinitely.
    static TWeakObjectPtr<UTexture2D> CachedTexture;
    UTexture2D* Texture = CachedTexture.Get();
    if (!Texture)
    {
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Origin"));
        if (Plugin)
        {
            Texture = FImageUtils::ImportFileAsTexture2D(FPaths::Combine(
                Plugin->GetBaseDir(),TEXT("Resources"),TEXT("Origin-Anchor-Actor-Billboard.PNG")));
            CachedTexture = Texture;
        }
    }
    if (Texture && EditorBillboard->Sprite != Texture) EditorBillboard->SetSprite(Texture);
    EditorBillboard->SetVisibility(bShowEditorBillboard && Texture != nullptr);
    EditorBillboard->MarkRenderStateDirty();
}
void AOriginAnchor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    RefreshEditorBillboard();
}
void AOriginAnchor::PostEditUndo()
{
    Super::PostEditUndo();
    RefreshEditorBillboard();
}
bool AOriginAnchor::IsOriginEditorComponent(const UActorComponent* Component) const
{
    return EditorBillboard && Component == EditorBillboard.Get();
}
#endif
