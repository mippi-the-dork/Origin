// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginAnchor.h"
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

bool AOriginAnchor::GetOriginPivotSlotDefinition(
    FName SlotName, FOriginPivotDefinition& OutDefinition) const
{
    if (const FOriginPivotSlot* Slot = OriginPivotConfiguration.FindSlot(SlotName))
    {
        OutDefinition = Slot->Definition;
        return true;
    }
    return false;
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
    OriginPivotConfiguration.SchemaVersion = 1;
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
