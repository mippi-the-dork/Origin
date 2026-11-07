// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// All fixtures live in an isolated editor-type world: never use an open map.
#include "OriginActorAssemblyAuthoring.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "OriginAnchor.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    struct FAssemblyFixture
    {
        FTestWorldWrapper TestWorld;
        UWorld* World = nullptr;
        AActor* Parent = nullptr;
        AOriginAnchor* Anchor = nullptr;
        AActor* ChildA = nullptr;
        AActor* ChildB = nullptr;
        AActor* Grandchild = nullptr;
        AActor* Unrelated = nullptr;

        AActor* MakeActor(const TCHAR* Name, const FVector& Location, const FRotator& Rotation)
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* Actor = World->SpawnActor<AActor>(Location, Rotation, Params);
            if (!Actor) return nullptr;
            Actor->SetActorLabel(Name);
            USceneComponent* Root = NewObject<USceneComponent>(Actor, NAME_None, RF_Transactional);
            if (!Root) return nullptr;
            Root->SetMobility(EComponentMobility::Movable);
            Actor->AddInstanceComponent(Root);
            Actor->SetRootComponent(Root);
            Root->RegisterComponent();
            Actor->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
            return Actor;
        }
        bool Setup(FAutomationTestBase& Test)
        {
            if (!Test.TestTrue(TEXT("Independent editor test world"), TestWorld.CreateTestWorld(EWorldType::Editor))) return false;
            World = TestWorld.GetTestWorld();
            if (!Test.TestNotNull(TEXT("Editor test world"), World)) return false;
            Parent = MakeActor(TEXT("Parent"), FVector(300, 20, 0), FRotator(0, 20, 0));
            ChildA = MakeActor(TEXT("ChildA"), FVector(460, 80, 10), FRotator(0, -15, 0));
            ChildB = MakeActor(TEXT("ChildB"), FVector(-90, 130, 35), FRotator(0, 12, 0));
            Grandchild = MakeActor(TEXT("Grandchild"), FVector(510, 100, 20), FRotator(0, 42, 0));
            Unrelated = MakeActor(TEXT("Unrelated"), FVector(-160, -60, 5), FRotator(0, -25, 0));
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Anchor = World->SpawnActor<AOriginAnchor>(FVector(360, 10, 5), FRotator(0, 45, 0), Params);
            if (!Test.TestNotNull(TEXT("Anchor"), Anchor) || !Test.TestNotNull(TEXT("Parent"), Parent) ||
                !Test.TestNotNull(TEXT("Child A"), ChildA) || !Test.TestNotNull(TEXT("Child B"), ChildB) ||
                !Test.TestNotNull(TEXT("Grandchild"), Grandchild) || !Test.TestNotNull(TEXT("Unrelated"), Unrelated)) return false;
            // AOriginAnchor defaults to Static, but these test actors deliberately
            // use Movable roots. A static child cannot attach beneath a Movable
            // parent: make the fixture a valid Unreal attachment hierarchy.
            if (!Test.TestNotNull(TEXT("Anchor root"), Anchor->GetRootComponent())) return false;
            Anchor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
            if (!Test.TestTrue(TEXT("Fixture Anchor root is Movable"),
                Anchor->GetRootComponent()->Mobility == EComponentMobility::Movable)) return false;
            // Initial structure: Parent > Anchor > ChildA > Grandchild; Parent > ChildB; Unrelated.
            return Test.TestTrue(TEXT("Attach anchor to Parent"), Anchor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform)) &&
                Test.TestTrue(TEXT("Attach ChildA to anchor"), ChildA->AttachToActor(Anchor, FAttachmentTransformRules::KeepWorldTransform)) &&
                Test.TestTrue(TEXT("Attach Grandchild to ChildA"), Grandchild->AttachToActor(ChildA, FAttachmentTransformRules::KeepWorldTransform)) &&
                Test.TestTrue(TEXT("Attach ChildB to Parent"), ChildB->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform));
        }
    };
    bool SameWorld(const AActor* Actor, const FTransform& Original)
    { return IsValid(Actor) && Actor->GetActorTransform().Equals(Original, 1.e-3); }

    // A separate native editor transaction buffer avoids changing the user's Undo stack.
    struct FScopedTestUndo
    {
        TStrongObjectPtr<UTransactor> Original;
        TStrongObjectPtr<UTransBuffer> Buffer;
        bool bInstalled = false;
        FScopedTestUndo()
        {
            if (!GEditor || !IsValid(GEditor->Trans.Get()) || GEditor->Trans->IsActive()) return;
            Original.Reset(GEditor->Trans.Get());
            UTransBuffer* Private = Cast<UTransBuffer>(GEditor->CreateTrans());
            if (!IsValid(Private)) { GEditor->Trans = Original.Get(); return; }
            Buffer.Reset(Private);
            GEditor->Trans = Private;
            bInstalled = true;
        }
        ~FScopedTestUndo()
        { if (bInstalled && GEditor) GEditor->Trans = Original.Get(); }
    };
}
#define ORIGIN_ASSEMBLY_TEST(Class, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_ASSEMBLY_TEST(FOriginActorAdopt, "Origin.Editor.Hierarchy.Actor.AdoptMultiplePreservesBranches")
bool FOriginActorAdopt::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    const FTransform A = F.ChildB->GetActorTransform(), B = F.Unrelated->GetActorTransform();
    const FTransform Other = F.ChildA->GetActorTransform(), Grand = F.Grandchild->GetActorTransform();
    FText Reason;
    if (!TestTrue(TEXT("Adopt independent Actors from different immediate parents"),
        OriginActorAssembly::Adopt(F.Anchor, {F.ChildB, F.Unrelated}, Reason))) { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("ChildB parent is Anchor"), F.ChildB->GetAttachParentActor() == F.Anchor);
    TestTrue(TEXT("Unrelated parent is Anchor"), F.Unrelated->GetAttachParentActor() == F.Anchor);
    TestTrue(TEXT("ChildB world transform retained"), SameWorld(F.ChildB, A));
    TestTrue(TEXT("Unrelated world transform retained"), SameWorld(F.Unrelated, B));
    TestTrue(TEXT("Existing branch root retained"), SameWorld(F.ChildA, Other));
    TestTrue(TEXT("Existing grandchild retained"), SameWorld(F.Grandchild, Grand));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorInsert, "Origin.Editor.Hierarchy.Actor.InsertPreservesParentAndDescendant")
bool FOriginActorInsert::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    const FTransform RootBefore = F.ChildA->GetActorTransform(), NestedBefore = F.Grandchild->GetActorTransform();
    FText Reason; AOriginAnchor* NewAnchor = nullptr;
    if (!TestTrue(TEXT("Insert above direct child"), OriginActorAssembly::Insert(F.ChildA, NewAnchor, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestNotNull(TEXT("Anchor created"), NewAnchor);
    TestTrue(TEXT("Inserted Anchor inherits Movable root mobility"),
        NewAnchor && NewAnchor->GetRootComponent() &&
        NewAnchor->GetRootComponent()->Mobility == EComponentMobility::Movable);
    TestTrue(TEXT("New anchor inherits old parent"), NewAnchor && NewAnchor->GetAttachParentActor() == F.Anchor);
    TestTrue(TEXT("Old child beneath new anchor"), F.ChildA->GetAttachParentActor() == NewAnchor);
    TestTrue(TEXT("Root retains world transform"), SameWorld(F.ChildA, RootBefore));
    TestTrue(TEXT("Nested descendant retains world transform"), SameWorld(F.Grandchild, NestedBefore));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorPromote, "Origin.Editor.Hierarchy.Actor.PromotePreservesPlacement")
bool FOriginActorPromote::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    const FTransform RootBefore = F.ChildA->GetActorTransform(), NestedBefore = F.Grandchild->GetActorTransform();
    FText Reason;
    if (!TestTrue(TEXT("Promote from Anchor to Parent"), OriginActorAssembly::Promote({F.ChildA}, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Promoted to Anchor's parent"), F.ChildA->GetAttachParentActor() == F.Parent);
    TestTrue(TEXT("Descendant retained its parent"), F.Grandchild->GetAttachParentActor() == F.ChildA);
    TestTrue(TEXT("Child retains placement"), SameWorld(F.ChildA, RootBefore));
    TestTrue(TEXT("Grandchild retains placement"), SameWorld(F.Grandchild, NestedBefore));
    TestTrue(TEXT("Anchor remains attached to Parent"), F.Anchor->GetAttachParentActor() == F.Parent);
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorDetach, "Origin.Editor.Hierarchy.Actor.DetachPreservesPlacement")
bool FOriginActorDetach::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    const FTransform RootBefore = F.ChildA->GetActorTransform(), NestedBefore = F.Grandchild->GetActorTransform();
    FText Reason;
    if (!TestTrue(TEXT("Detach from Anchor to level root"), OriginActorAssembly::Detach({F.ChildA}, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("No attachment parent"), F.ChildA->GetAttachParentActor() == nullptr);
    TestTrue(TEXT("Descendant remains below ChildA"), F.Grandchild->GetAttachParentActor() == F.ChildA);
    TestTrue(TEXT("Child retains placement"), SameWorld(F.ChildA, RootBefore));
    TestTrue(TEXT("Grandchild retains placement"), SameWorld(F.Grandchild, NestedBefore));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorReject, "Origin.Editor.Hierarchy.Actor.RejectCycleWithoutChange")
bool FOriginActorReject::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    const FTransform Before = F.Parent->GetActorTransform();
    FText Reason;
    TestFalse(TEXT("Reject attaching ancestor under its descendant"),
        OriginActorAssembly::Adopt(F.Anchor, {F.Parent}, Reason));
    TestTrue(TEXT("Action gave a reason"), !Reason.IsEmpty());
    TestTrue(TEXT("Parent is still root"), F.Parent->GetAttachParentActor() == nullptr);
    TestTrue(TEXT("Anchor retains Parent"), F.Anchor->GetAttachParentActor() == F.Parent);
    TestTrue(TEXT("No transform changed"), SameWorld(F.Parent, Before));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorRejectScale, "Origin.Editor.Hierarchy.Actor.RejectNonuniformScale")
bool FOriginActorRejectScale::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    F.Unrelated->SetActorScale3D(FVector(2, 1, 1));
    const FTransform Before = F.Unrelated->GetActorTransform();
    FText Reason;
    TestFalse(TEXT("Reject nonuniform Actor scale"), OriginActorAssembly::Adopt(F.Anchor, {F.Unrelated}, Reason));
    TestTrue(TEXT("Action gave a reason"), !Reason.IsEmpty());
    TestNull(TEXT("No new attachment"), F.Unrelated->GetAttachParentActor());
    TestTrue(TEXT("World transform unchanged"), SameWorld(F.Unrelated, Before));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorTransaction, "Origin.Editor.Hierarchy.Actor.Transaction.AdoptUndoRedo")
bool FOriginActorTransaction::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    FScopedTestUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Isolated editor transaction buffer"), Buffer)) return false;
    const FTransform Before = F.ChildB->GetActorTransform();
    for (int32 Cycle = 0; Cycle < 3; ++Cycle)
    {
        FText Reason;
        if (!TestTrue(TEXT("Adopt into Anchor"), OriginActorAssembly::Adopt(F.Anchor, {F.ChildB}, Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestTrue(TEXT("Adopt attached"), F.ChildB->GetAttachParentActor() == F.Anchor)) return false;
        if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo restores original parent"), F.ChildB->GetAttachParentActor() == F.Parent)) return false;
        if (!TestTrue(TEXT("Undo retains transform"), SameWorld(F.ChildB, Before))) return false;
        if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo restores new parent"), F.ChildB->GetAttachParentActor() == F.Anchor)) return false;
        if (!TestTrue(TEXT("Redo retains transform"), SameWorld(F.ChildB, Before))) return false;
        if (!TestTrue(TEXT("Undo for next iteration"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Ready for next adopt"), F.ChildB->GetAttachParentActor() == F.Parent)) return false;
    }
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorComponentParent, "Origin.Editor.Hierarchy.Actor.InsertRetainsComponentParent")
bool FOriginActorComponentParent::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    USceneComponent* Hook = NewObject<USceneComponent>(F.Parent, NAME_None, RF_Transactional);
    if (!TestNotNull(TEXT("Parent's non-root attachment component"), Hook)) return false;
    Hook->SetMobility(EComponentMobility::Movable);
    F.Parent->AddInstanceComponent(Hook);
    Hook->SetupAttachment(F.Parent->GetRootComponent());
    Hook->RegisterComponent();
    Hook->SetRelativeLocation(FVector(20, 15, 0));
    if (!TestTrue(TEXT("ChildA can attach to non-root component"),
        F.ChildA->AttachToComponent(Hook, FAttachmentTransformRules::KeepWorldTransform))) return false;
    const FTransform Before = F.ChildA->GetActorTransform();
    const FTransform GrandBefore = F.Grandchild->GetActorTransform();
    FText Reason; AOriginAnchor* NewAnchor = nullptr;
    if (!TestTrue(TEXT("Insert above non-root component attachment"),
        OriginActorAssembly::Insert(F.ChildA, NewAnchor, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Inserted Anchor matches Movable child beneath non-root component"),
        NewAnchor && NewAnchor->GetRootComponent() &&
        NewAnchor->GetRootComponent()->Mobility == EComponentMobility::Movable);
    TestTrue(TEXT("New Anchor attached to original non-root component"),
        NewAnchor && NewAnchor->GetRootComponent()->GetAttachParent() == Hook);
    TestTrue(TEXT("ChildA attached to inserted Anchor"), F.ChildA->GetAttachParentActor() == NewAnchor);
    TestTrue(TEXT("ChildA placement unchanged"), SameWorld(F.ChildA, Before));
    TestTrue(TEXT("Grandchild placement unchanged"), SameWorld(F.Grandchild, GrandBefore));
    return true;
}

// The default Static Anchor still needs to work for a genuinely Static branch.
// This also catches regressions that blindly force all newly inserted Anchors Movable.
ORIGIN_ASSEMBLY_TEST(FOriginActorInsertStatic, "Origin.Editor.Hierarchy.Actor.InsertPreservesStaticRootMobility")
bool FOriginActorInsertStatic::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    F.ChildB->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    if (!TestNull(TEXT("Static fixture actor is now a level root"), F.ChildB->GetAttachParentActor())) return false;
    F.ChildB->GetRootComponent()->SetMobility(EComponentMobility::Static);
    if (!TestTrue(TEXT("Static fixture actor root is Static"),
        F.ChildB->GetRootComponent()->Mobility == EComponentMobility::Static)) return false;
    const FTransform Before = F.ChildB->GetActorTransform();
    FText Reason;
    AOriginAnchor* NewAnchor = nullptr;
    if (!TestTrue(TEXT("Insert Anchor above Static root"), OriginActorAssembly::Insert(F.ChildB, NewAnchor, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Inserted Anchor remains Static"), NewAnchor && NewAnchor->GetRootComponent() &&
        NewAnchor->GetRootComponent()->Mobility == EComponentMobility::Static);
    TestTrue(TEXT("Inserted Anchor remains level root"), NewAnchor && NewAnchor->GetAttachParentActor() == nullptr);
    TestTrue(TEXT("Static child is attached beneath inserted Anchor"), F.ChildB->GetAttachParentActor() == NewAnchor);
    TestTrue(TEXT("Static child transform preserved"), SameWorld(F.ChildB, Before));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorInsertTransaction, "Origin.Editor.Hierarchy.Actor.Transaction.InsertUndoReinsertRedo")
bool FOriginActorInsertTransaction::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    FScopedTestUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Isolated native editor transaction buffer"), Buffer)) return false;
    const FTransform Before = F.ChildA->GetActorTransform();
    const FTransform NestedBefore = F.Grandchild->GetActorTransform();
    for (int32 Cycle = 0; Cycle < 3; ++Cycle)
    {
        FText Reason; AOriginAnchor* NewAnchor = nullptr;
        if (!TestTrue(TEXT("Insert Anchor"), OriginActorAssembly::Insert(F.ChildA, NewAnchor, Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestTrue(TEXT("Inserted correct parent"), F.ChildA->GetAttachParentActor() == NewAnchor)) return false;
        if (!TestTrue(TEXT("Undo insertion"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo restores original Anchor parent"), F.ChildA->GetAttachParentActor() == F.Anchor)) return false;
        if (!TestTrue(TEXT("Undo retains descendant placement"), SameWorld(F.Grandchild, NestedBefore))) return false;
        if (!TestTrue(TEXT("Redo insertion"), Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo reattaches under inserted Anchor"),
            F.ChildA->GetAttachParentActor() == NewAnchor)) return false;
        if (!TestTrue(TEXT("Redo retains child placement"), SameWorld(F.ChildA, Before))) return false;
        if (!TestTrue(TEXT("Undo before next insertion"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Original hierarchy restored"), F.ChildA->GetAttachParentActor() == F.Anchor)) return false;
    }
    return true;
}


// Alpha 5.3: one invalid member must reject the entire batch before any attachment.
ORIGIN_ASSEMBLY_TEST(FOriginActorRejectBatchAtomic, "Origin.Editor.Hierarchy.Actor.Robustness.MixedValidInvalidBatchIsAtomic")
bool FOriginActorRejectBatchAtomic::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    F.Unrelated->SetActorScale3D(FVector(1.8, 1, 1));
    const FTransform ValidBefore = F.ChildB->GetActorTransform();
    const FTransform InvalidBefore = F.Unrelated->GetActorTransform();
    FText Reason;
    TestFalse(TEXT("Whole batch rejected"), OriginActorAssembly::Adopt(F.Anchor, {F.ChildB, F.Unrelated}, Reason));
    TestTrue(TEXT("Rejection gives reason"), !Reason.IsEmpty());
    TestTrue(TEXT("Valid member remains under original parent"), F.ChildB->GetAttachParentActor() == F.Parent);
    TestNull(TEXT("Invalid member remains unattached"), F.Unrelated->GetAttachParentActor());
    TestTrue(TEXT("Valid member unchanged"), SameWorld(F.ChildB, ValidBefore));
    TestTrue(TEXT("Invalid member unchanged"), SameWorld(F.Unrelated, InvalidBefore));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorRejectSelectedBranch, "Origin.Editor.Hierarchy.Actor.Robustness.NestedSelectionIsAtomic")
bool FOriginActorRejectSelectedBranch::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    if (!TestTrue(TEXT("Make nested branch"),
        F.Unrelated->AttachToActor(F.ChildB, FAttachmentTransformRules::KeepWorldTransform))) return false;
    const FTransform ParentBefore = F.ChildB->GetActorTransform();
    const FTransform NestedBefore = F.Unrelated->GetActorTransform();
    FText Reason;
    TestFalse(TEXT("Cannot select parent and its descendant in one adoption"),
        OriginActorAssembly::Adopt(F.Anchor, {F.ChildB, F.Unrelated}, Reason));
    TestTrue(TEXT("Failure explains invalid selection"), !Reason.IsEmpty());
    TestTrue(TEXT("Parent unchanged"), F.ChildB->GetAttachParentActor() == F.Parent);
    TestTrue(TEXT("Descendant unchanged"), F.Unrelated->GetAttachParentActor() == F.ChildB);
    TestTrue(TEXT("World placement of root preserved"), SameWorld(F.ChildB, ParentBefore));
    TestTrue(TEXT("World placement of descendant preserved"), SameWorld(F.Unrelated, NestedBefore));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorMobilityReject, "Origin.Editor.Hierarchy.Actor.Robustness.StaticChildToMovableTargetRejected")
bool FOriginActorMobilityReject::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    F.Unrelated->GetRootComponent()->SetMobility(EComponentMobility::Static);
    const FTransform Before = F.Unrelated->GetActorTransform();
    FText Reason;
    TestFalse(TEXT("Reject before native AttachTo can emit a mobility warning"),
        OriginActorAssembly::CanAdopt(F.Anchor, {F.Unrelated}, Reason));
    TestTrue(TEXT("Reason identifies mobility"), Reason.ToString().Contains(TEXT("mobility")) ||
        Reason.ToString().Contains(TEXT("mobile")));
    TestFalse(TEXT("Actual operation rejects"), OriginActorAssembly::Adopt(F.Anchor, {F.Unrelated}, Reason));
    TestNull(TEXT("No attachment was made"), F.Unrelated->GetAttachParentActor());
    TestTrue(TEXT("Static actor did not move"), SameWorld(F.Unrelated, Before));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorStaticAdopt, "Origin.Editor.Hierarchy.Actor.Robustness.StaticActorToStaticAnchor")
bool FOriginActorStaticAdopt::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    F.Anchor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    F.Anchor->GetRootComponent()->SetMobility(EComponentMobility::Static);
    F.Unrelated->GetRootComponent()->SetMobility(EComponentMobility::Static);
    const FTransform Before = F.Unrelated->GetActorTransform();
    FText Reason;
    if (!TestTrue(TEXT("Static Actor may attach under Static Anchor"),
        OriginActorAssembly::Adopt(F.Anchor, {F.Unrelated}, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Static Actor now attached"), F.Unrelated->GetAttachParentActor() == F.Anchor);
    TestTrue(TEXT("Static Actor placement unchanged"), SameWorld(F.Unrelated, Before));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorDeepBranch, "Origin.Editor.Hierarchy.Actor.Robustness.FourLevelBranchInsertion")
bool FOriginActorDeepBranch::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    AActor* Depth3 = F.MakeActor(TEXT("Depth3"), FVector(560, 105, 50), FRotator(0, 8, 0));
    AActor* Depth4 = F.MakeActor(TEXT("Depth4"), FVector(620, -20, 60), FRotator(0, -32, 0));
    if (!TestNotNull(TEXT("Third-level actor"), Depth3) ||
        !TestNotNull(TEXT("Fourth-level actor"), Depth4)) return false;
    if (!TestTrue(TEXT("Build third level"), Depth3->AttachToActor(F.Grandchild, FAttachmentTransformRules::KeepWorldTransform)) ||
        !TestTrue(TEXT("Build fourth level"), Depth4->AttachToActor(Depth3, FAttachmentTransformRules::KeepWorldTransform))) return false;
    const FTransform A = F.ChildA->GetActorTransform(), B = F.Grandchild->GetActorTransform();
    const FTransform C = Depth3->GetActorTransform(), D = Depth4->GetActorTransform();
    FText Reason; AOriginAnchor* Inserted = nullptr;
    if (!TestTrue(TEXT("Insert new Anchor above four-level branch"),
        OriginActorAssembly::Insert(F.ChildA, Inserted, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("New anchor attached beneath old anchor"), Inserted && Inserted->GetAttachParentActor() == F.Anchor);
    TestTrue(TEXT("Top child beneath new anchor"), F.ChildA->GetAttachParentActor() == Inserted);
    TestTrue(TEXT("Second level preserved"), F.Grandchild->GetAttachParentActor() == F.ChildA);
    TestTrue(TEXT("Third level preserved"), Depth3->GetAttachParentActor() == F.Grandchild);
    TestTrue(TEXT("Fourth level preserved"), Depth4->GetAttachParentActor() == Depth3);
    TestTrue(TEXT("All four transforms preserved"), SameWorld(F.ChildA,A) && SameWorld(F.Grandchild,B) &&
        SameWorld(Depth3,C) && SameWorld(Depth4,D));
    return true;
}

ORIGIN_ASSEMBLY_TEST(FOriginActorPromotionTransaction, "Origin.Editor.Hierarchy.Actor.Robustness.MultiActorPromoteUndoRedo")
bool FOriginActorPromotionTransaction::RunTest(const FString&)
{
    FAssemblyFixture F;
    if (!F.Setup(*this)) return false;
    if (!TestTrue(TEXT("Second child attaches under Anchor"),
        F.ChildB->AttachToActor(F.Anchor, FAttachmentTransformRules::KeepWorldTransform))) return false;
    const FTransform BeforeA = F.ChildA->GetActorTransform();
    const FTransform BeforeB = F.ChildB->GetActorTransform();
    const FTransform BeforeNested = F.Grandchild->GetActorTransform();
    FScopedTestUndo Undo;
    UTransBuffer* Buffer = Undo.bInstalled ? Undo.Buffer.Get() : nullptr;
    if (!TestNotNull(TEXT("Isolated native Undo"), Buffer)) return false;
    for (int32 Cycle=0; Cycle<2; ++Cycle)
    {
        FText Reason;
        if (!TestTrue(TEXT("Promote two children as one operation"),
            OriginActorAssembly::Promote({F.ChildA,F.ChildB}, Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestTrue(TEXT("Both promoted to original parent"),
            F.ChildA->GetAttachParentActor() == F.Parent && F.ChildB->GetAttachParentActor() == F.Parent)) return false;
        if (!TestTrue(TEXT("Undo one transaction"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo restores both parent relationships"),
            F.ChildA->GetAttachParentActor() == F.Anchor && F.ChildB->GetAttachParentActor() == F.Anchor)) return false;
        if (!TestTrue(TEXT("Redo one transaction"), Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo promotes both"),
            F.ChildA->GetAttachParentActor() == F.Parent && F.ChildB->GetAttachParentActor() == F.Parent)) return false;
        if (!TestTrue(TEXT("All descendant world transforms preserved"),
            SameWorld(F.ChildA, BeforeA) && SameWorld(F.ChildB, BeforeB) && SameWorld(F.Grandchild, BeforeNested))) return false;
        if (!TestTrue(TEXT("Undo before next cycle"), Buffer->Undo(true))) return false;
    }
    return true;
}

#undef ORIGIN_ASSEMBLY_TEST
#endif
