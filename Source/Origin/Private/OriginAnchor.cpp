#include "OriginAnchor.h"
#include "Components/SceneComponent.h"

AOriginAnchor::AOriginAnchor()
{
    PrimaryActorTick.bCanEverTick = false;
    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
    Root->SetMobility(EComponentMobility::Static);
    SetRootComponent(Root);
    SetActorEnableCollision(false);
    // No editor-only actor flag: the root and attachment hierarchy must survive cooking.
}
