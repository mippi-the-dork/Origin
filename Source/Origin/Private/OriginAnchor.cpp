// Copyright Epic Games, Inc. All Rights Reserved.

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
