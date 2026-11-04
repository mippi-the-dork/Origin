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
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    struct FFixture
    {
        TStrongObjectPtr<UBlueprint> Blueprint;
        bool Build(FAutomationTestBase& Test)
        {
            const FName UniqueName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(),
                TEXT("OriginHierarchyAuthoringFixture"));
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
                GetTransientPackage(), UniqueName, BPTYPE_Normal, TEXT("OriginHierarchyTests"));
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

#undef ORIGIN_HIERARCHY_TEST
#endif
