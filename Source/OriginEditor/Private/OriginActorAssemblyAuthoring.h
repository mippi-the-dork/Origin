// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class AActor;
class AOriginAnchor;

/** Level Editor actor attachment operations. They do not modify Blueprint SCS trees.
 * All entry points preflight the full affected branch before opening a transaction.
 */
namespace OriginActorAssembly
{
    /** Reparent one or more loaded Actors under an existing Origin Anchor. */
    bool CanAdopt(AOriginAnchor* Target, const TArray<AActor*>& Actors, FText& Reason);
    bool Adopt(AOriginAnchor* Target, const TArray<AActor*>& Actors, FText& Reason);

    /** Insert an Origin Anchor above one Actor, preserving its original parent component/socket. */
    bool CanInsert(AActor* Child, FText& Reason);
    bool Insert(AActor* Child, AOriginAnchor*& NewAnchor, FText& Reason);

    /** Move direct children of an Origin Anchor to the Anchor's own parent component/socket. */
    bool CanPromote(const TArray<AActor*>& Children, FText& Reason);
    bool Promote(const TArray<AActor*>& Children, FText& Reason);

    /** Detach direct children of an Origin Anchor to the level root. */
    bool CanDetach(const TArray<AActor*>& Children, FText& Reason);
    bool Detach(const TArray<AActor*>& Children, FText& Reason);
}
