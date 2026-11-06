// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginHierarchyAuthoring.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "OriginPivotComponent.h"
#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    struct FFixture
    {
        TStrongObjectPtr<UBlueprint> Blueprint;
        bool Build(FAutomationTestBase& Test, bool bUseTransactionPackage = false)
        {
            // The transaction test needs a non-transient, in-memory package so the
            // production FScopedTransaction is actually enabled. Never save this package.
            UPackage* OuterPackage = bUseTransactionPackage
                ? CreatePackage(*FString::Printf(TEXT("/Temp/OriginHierarchyUndo_%s"),
                    *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
                : GetTransientPackage();
            // Blueprint compilation creates SKEL_ and REINST_ classes under
            // /Engine/Transient. Make the Blueprint name globally unique (not
            // just unique among Blueprints in its Outer) so an earlier test's
            // reinstanced class cannot collide with the next fixture's SKEL CDO.
            const FName UniqueName(*FString::Printf(TEXT("OriginHierarchyQA_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
                OuterPackage, UniqueName, BPTYPE_Normal, TEXT("OriginHierarchyTests"));
            Blueprint.Reset(BP);
            if (!Test.TestNotNull(TEXT("Engine-initialized transient Blueprint"), BP)) return false;
            USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
            if (!Test.TestNotNull(TEXT("Initialized SCS"), SCS)) return false;
            USCS_Node* Root = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Root"));
            USCS_Node* Pivot = SCS->CreateNode(UOriginPivotComponent::StaticClass(), TEXT("MainPivot"));
            USCS_Node* Sibling = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Sibling"));
            USCS_Node* Child = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Child"));
            USCS_Node* Nested = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Nested"));
            if (!Test.TestNotNull(TEXT("Root node"), Root) ||
                !Test.TestNotNull(TEXT("Pivot node"), Pivot) ||
                !Test.TestNotNull(TEXT("Sibling node"), Sibling) ||
                !Test.TestNotNull(TEXT("Child node"), Child) ||
                !Test.TestNotNull(TEXT("Nested node"), Nested)) return false;
            SCS->AddNode(Root);
            Root->AddChildNode(Pivot, true);
            Root->AddChildNode(Sibling, true);
            Pivot->AddChildNode(Child, true);
            Child->AddChildNode(Nested, true);
            Cast<USceneComponent>(Pivot->ComponentTemplate.Get())->SetRelativeTransform(
                FTransform(FRotator(0, 30, 0), FVector(45, 10, 5), FVector::OneVector));
            Cast<USceneComponent>(Sibling->ComponentTemplate.Get())->SetRelativeTransform(
                FTransform(FRotator(0, -20, 0), FVector(160, -10, 30), FVector::OneVector));
            Cast<USceneComponent>(Child->ComponentTemplate.Get())->SetRelativeTransform(
                FTransform(FRotator(0, 15, 0), FVector(-20, 60, 0), FVector::OneVector));
            Cast<USceneComponent>(Nested->ComponentTemplate.Get())->SetRelativeLocation(FVector(25, 0, 2));
            FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
            return Test.TestNotNull(TEXT("Compiled generated class"), BP->GeneratedClass.Get());
        }
        USimpleConstructionScript* SCS() const { return Blueprint.IsValid() ? Blueprint->SimpleConstructionScript : nullptr; }
        USCS_Node* Node(FName Name) const { return SCS() ? SCS()->FindSCSNode(Name) : nullptr; }
        UOriginPivotComponent* Pivot() const
        {
            USCS_Node* P = Node(TEXT("MainPivot"));
            return P ? Cast<UOriginPivotComponent>(P->ComponentTemplate.Get()) : nullptr;
        }
        FTransform Relative(FName Name) const
        {
            USCS_Node* N = Node(Name);
            const USceneComponent* C = N ? Cast<USceneComponent>(N->ComponentTemplate.Get()) : nullptr;
            return C ? C->GetRelativeTransform() : FTransform::Identity;
        }
    };
}

#define ORIGIN_HIERARCHY_TEST(Class, Name) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Name, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_HIERARCHY_TEST(FOriginHierarchyAdoptSibling, "Origin.Editor.Hierarchy.Blueprint.AdoptSiblingPreservesPlacement")
bool FOriginHierarchyAdoptSibling::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    if (!Fixture.Build(*this)) return false;
    const FTransform Before = Fixture.Relative(TEXT("Sibling"));
    FText Reason;
    if (!TestTrue(TEXT("Adopt existing sibling"), FOriginHierarchyAuthoring::AdoptSibling(Fixture.Pivot(), TEXT("Sibling"), Reason)))
    { AddError(Reason.ToString()); return false; }
    if (!TestTrue(TEXT("Sibling now directly beneath pivot"),
        Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Sibling"))) == Fixture.Node(TEXT("MainPivot")))) return false;
    TestTrue(TEXT("Sibling's placement in original parent frame is unchanged"),
        (Fixture.Relative(TEXT("Sibling")) * Fixture.Relative(TEXT("MainPivot"))).Equals(Before, 1.e-3));
    TestEqual(TEXT("SCS node count unchanged"), Fixture.SCS()->GetAllNodes().Num(), 5);
    return true;
}

ORIGIN_HIERARCHY_TEST(FOriginHierarchyPromoteChild, "Origin.Editor.Hierarchy.Blueprint.PromoteChildPreservesPlacement")
bool FOriginHierarchyPromoteChild::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    if (!Fixture.Build(*this)) return false;
    const FTransform Before = Fixture.Relative(TEXT("Child")) * Fixture.Relative(TEXT("MainPivot"));
    const FTransform NestedBefore = Fixture.Relative(TEXT("Nested")) * Before;
    FText Reason;
    if (!TestTrue(TEXT("Promote direct child"), FOriginHierarchyAuthoring::PromoteChild(Fixture.Pivot(), TEXT("Child"), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Child is now sibling of pivot"),
        Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Child"))) == Fixture.Node(TEXT("Root")));
    TestTrue(TEXT("Child remains at same parent-frame placement"),
        Fixture.Relative(TEXT("Child")).Equals(Before, 1.e-3));
    TestTrue(TEXT("Nested grandchild remains in place"),
        (Fixture.Relative(TEXT("Nested")) * Fixture.Relative(TEXT("Child"))).Equals(NestedBefore, 1.e-3));
    return true;
}

ORIGIN_HIERARCHY_TEST(FOriginHierarchyInsertPivot, "Origin.Editor.Hierarchy.Blueprint.InsertPivotPreservesBranch")
bool FOriginHierarchyInsertPivot::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    if (!Fixture.Build(*this)) return false;
    const FTransform Before = Fixture.Relative(TEXT("Child"));
    const FTransform NestedBefore = Fixture.Relative(TEXT("Nested")) * Before;
    FName NewVariable;
    FText Reason;
    if (!TestTrue(TEXT("Insert Origin pivot above authored child"),
        FOriginHierarchyAuthoring::InsertPivotAboveChild(Fixture.Pivot(), TEXT("Child"), NewVariable, Reason)))
    { AddError(Reason.ToString()); return false; }
    USCS_Node* NewNode = Fixture.Node(NewVariable);
    if (!TestNotNull(TEXT("Inserted SCS node exists"), NewNode)) return false;
    TestTrue(TEXT("Inserted node has Origin Pivot template"),
        NewNode->ComponentTemplate->IsA<UOriginPivotComponent>());
    TestTrue(TEXT("New node is beneath original pivot"),
        Fixture.SCS()->FindParentNode(NewNode) == Fixture.Node(TEXT("MainPivot")));
    TestTrue(TEXT("Child is beneath new pivot"),
        Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Child"))) == NewNode);
    TestTrue(TEXT("Original child placement preserved"),
        (Fixture.Relative(TEXT("Child")) * Fixture.Relative(NewVariable)).Equals(Before, 1.e-3));
    TestTrue(TEXT("Nested branch placement preserved"),
        (Fixture.Relative(TEXT("Nested")) * Fixture.Relative(TEXT("Child")) * Fixture.Relative(NewVariable)).Equals(NestedBefore, 1.e-3));
    TestEqual(TEXT("Only one new SCS node"), Fixture.SCS()->GetAllNodes().Num(), 6);
    return true;
}

ORIGIN_HIERARCHY_TEST(FOriginHierarchyCompile, "Origin.Editor.Hierarchy.Blueprint.CompileRetainsTopology")
bool FOriginHierarchyCompile::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    if (!Fixture.Build(*this)) return false;
    const FTransform Before = Fixture.Relative(TEXT("Sibling"));
    FText Reason;
    if (!TestTrue(TEXT("Adopt before compile"), FOriginHierarchyAuthoring::AdoptSibling(Fixture.Pivot(), TEXT("Sibling"), Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    if (!TestNotNull(TEXT("Compiled sibling node"), Fixture.Node(TEXT("Sibling")))) return false;
    TestTrue(TEXT("SCS parent relationship survives compilation"),
        Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Sibling"))) == Fixture.Node(TEXT("MainPivot")));
    TestTrue(TEXT("Compiled relative transforms preserve prior placement"),
        (Fixture.Relative(TEXT("Sibling")) * Fixture.Relative(TEXT("MainPivot"))).Equals(Before, 1.e-3));
    return true;
}

ORIGIN_HIERARCHY_TEST(FOriginHierarchyRejection, "Origin.Editor.Hierarchy.Blueprint.RejectionIsAtomic")
bool FOriginHierarchyRejection::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    if (!Fixture.Build(*this)) return false;
    const FTransform Before = Fixture.Relative(TEXT("Child"));
    const int32 Count = Fixture.SCS()->GetAllNodes().Num();
    FText Reason;
    TestFalse(TEXT("Reject reparent of nonexistent sibling"),
        FOriginHierarchyAuthoring::AdoptSibling(Fixture.Pivot(), TEXT("DoesNotExist"), Reason));
    TestFalse(TEXT("Reject non-direct child promotion"),
        FOriginHierarchyAuthoring::PromoteChild(Fixture.Pivot(), TEXT("Nested"), Reason));
    FName Inserted;
    TestFalse(TEXT("Reject insertion against nonexistent direct child"),
        FOriginHierarchyAuthoring::InsertPivotAboveChild(Fixture.Pivot(), TEXT("Sibling"), Inserted, Reason));
    USceneComponent* ChildTemplate = Cast<USceneComponent>(Fixture.Node(TEXT("Child"))->ComponentTemplate.Get());
    if (!TestNotNull(TEXT("Child template for absolute transform rejection"), ChildTemplate)) return false;
    ChildTemplate->SetUsingAbsoluteLocation(true);
    TestFalse(TEXT("Reject component using absolute location"),
        FOriginHierarchyAuthoring::PromoteChild(Fixture.Pivot(), TEXT("Child"), Reason));
    ChildTemplate->SetUsingAbsoluteLocation(false);
    TestTrue(TEXT("Rejection reports a reason"), !Reason.IsEmpty());
    TestTrue(TEXT("Rejected operations retain original parent"),
        Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Child"))) == Fixture.Node(TEXT("MainPivot")));
    TestTrue(TEXT("Rejected operations retain transforms"), Fixture.Relative(TEXT("Child")).Equals(Before));
    TestEqual(TEXT("No nodes added or removed by rejection"), Fixture.SCS()->GetAllNodes().Num(), Count);
    return true;
}

/** Use a private native transaction buffer. This must never consume or reset the
 * user's existing editor Undo stack, even if the automation fails early.
 */
namespace
{
    struct FOriginIsolatedUndoBuffer
    {
        TStrongObjectPtr<UTransactor> Original;
        TStrongObjectPtr<UTransBuffer> Isolated;
        bool bInstalled = false;

        FOriginIsolatedUndoBuffer()
        {
            if (!GEditor || !IsValid(GEditor->Trans.Get()) || GEditor->Trans->IsActive())
            {
                return;
            }
            Original.Reset(GEditor->Trans.Get());
            // Use Unreal's editor transaction factory to initialize the private
            // buffer exactly as the editor initializes its regular Undo system.
            UTransBuffer* Buffer = Cast<UTransBuffer>(GEditor->CreateTrans());
            if (!IsValid(Buffer))
            {
                GEditor->Trans = Original.Get();
                return;
            }
            Isolated.Reset(Buffer);
            GEditor->Trans = Buffer;
            bInstalled = true;
        }
        ~FOriginIsolatedUndoBuffer()
        {
            if (bInstalled && GEditor)
            {
                GEditor->Trans = Original.Get();
            }
        }
        UTransBuffer* Get() const { return bInstalled ? Isolated.Get() : nullptr; }
    };
}

ORIGIN_HIERARCHY_TEST(FOriginHierarchyInsertionUndoCycle,
    "Origin.Editor.Hierarchy.Blueprint.Transaction.InsertUndoReinsertRedo")
bool FOriginHierarchyInsertionUndoCycle::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    if (!Fixture.Build(*this, true)) return false;
    FOriginIsolatedUndoBuffer ScopedTransactions;
    UTransBuffer* Buffer = ScopedTransactions.Get();
    if (!TestNotNull(TEXT("Isolated native transaction buffer, no editor Undo history changed"), Buffer)) return false;

    const FTransform OriginalChild = Fixture.Relative(TEXT("Child"));
    const FTransform OriginalNested = Fixture.Relative(TEXT("Nested")) * OriginalChild;
    const int32 OriginalNodeCount = Fixture.SCS()->GetAllNodes().Num();
    const USCS_Node* OriginalParent = Fixture.Node(TEXT("MainPivot"));

    auto CheckOriginal = [this, &Fixture, &OriginalChild, &OriginalNested, OriginalNodeCount, OriginalParent](const FString& Stage) -> bool
    {
        const USCS_Node* ChildNode = Fixture.Node(TEXT("Child"));
        if (!TestNotNull(*FString::Printf(TEXT("%s: child exists"), *Stage), ChildNode)) return false;
        const bool bParent = Fixture.SCS()->FindParentNode(const_cast<USCS_Node*>(ChildNode)) == OriginalParent;
        const bool bTransform = Fixture.Relative(TEXT("Child")).Equals(OriginalChild, 1.e-3);
        const bool bNested = (Fixture.Relative(TEXT("Nested")) * Fixture.Relative(TEXT("Child"))).Equals(OriginalNested, 1.e-3);
        const bool bCount = Fixture.SCS()->GetAllNodes().Num() == OriginalNodeCount;
        TestTrue(*FString::Printf(TEXT("%s: original parent"), *Stage), bParent);
        TestTrue(*FString::Printf(TEXT("%s: original transform"), *Stage), bTransform);
        TestTrue(*FString::Printf(TEXT("%s: nested branch preserved"), *Stage), bNested);
        TestTrue(*FString::Printf(TEXT("%s: no orphan SCS nodes"), *Stage), bCount);
        return bParent && bTransform && bNested && bCount;
    };

    for (int32 Cycle = 0; Cycle < 4; ++Cycle)
    {
        FText Reason;
        FName NewName;
        if (!TestTrue(*FString::Printf(TEXT("Cycle %d: insert"), Cycle),
            FOriginHierarchyAuthoring::InsertPivotAboveChild(Fixture.Pivot(), TEXT("Child"), NewName, Reason)))
        {
            AddError(Reason.ToString());
            return false;
        }
        const int32 RecordedTransactions = Buffer->GetQueueLength();
        const USCS_Node* Added = Fixture.Node(NewName);
        if (!TestNotNull(TEXT("New SCS node registered"), Added)) return false;
        const FGuid AddedGuid = Added->VariableGuid;
        TestTrue(TEXT("Child now attached to inserted pivot"),
            Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Child"))) == Added);
        TestTrue(TEXT("Only one node added"), Fixture.SCS()->GetAllNodes().Num() == OriginalNodeCount + 1);
        if (!TestEqual(TEXT("Insertion records exactly one native Undo transaction"), RecordedTransactions, 1)) return false;

        if (!TestTrue(TEXT("Undo insertion"), Buffer->Undo(true))) return false;
        if (!CheckOriginal(FString::Printf(TEXT("Cycle %d after Undo"), Cycle))) return false;
        if (!TestTrue(TEXT("Redo insertion"), Buffer->Redo())) return false;
        const USCS_Node* RedoneNode = Fixture.Node(NewName);
        if (!TestNotNull(TEXT("Redone SCS node exists"), RedoneNode)) return false;
        TestTrue(TEXT("Redo retains new pivot variable GUID"), RedoneNode->VariableGuid == AddedGuid);
        TestTrue(TEXT("Redo restores inserted parent"),
            Fixture.SCS()->FindParentNode(Fixture.Node(TEXT("Child"))) == RedoneNode);
        if (!TestTrue(TEXT("Redo preserves placement"),
            (Fixture.Relative(TEXT("Child")) * Fixture.Relative(NewName)).Equals(OriginalChild, 1.e-3))) return false;
        if (!TestTrue(TEXT("Undo redo-insertion"), Buffer->Undo(true))) return false;
        if (!CheckOriginal(FString::Printf(TEXT("Cycle %d after second Undo"), Cycle))) return false;
    }
    return true;
}

#undef ORIGIN_HIERARCHY_TEST
#endif
