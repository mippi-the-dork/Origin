// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginHierarchyAuthoring.h"
#include <initializer_list>

#if WITH_DEV_AUTOMATION_TESTS
#include "OriginPivotComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/SavePackage.h"
#include "UObject/PackageReload.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    TArray<FName> Names(std::initializer_list<const TCHAR*> Items)
    {
        TArray<FName> Result;
        for (const TCHAR* Item : Items) Result.Add(FName(Item));
        return Result;
    }

    struct FBatchFixture
    {
        TStrongObjectPtr<UBlueprint> Blueprint;
        bool Build(FAutomationTestBase& Test, bool bTransactional = false,
            UPackage* ExplicitPackage = nullptr, FName ExplicitAssetName = NAME_None,
            UClass* FixtureParentClass = nullptr)
        {
            UPackage* Package = ExplicitPackage ? ExplicitPackage : (bTransactional
                ? CreatePackage(*FString::Printf(TEXT("/Temp/OriginBatchUndo_%s"),
                    *FGuid::NewGuid().ToString(EGuidFormats::Digits))) : GetTransientPackage());
            const FName BPName = ExplicitAssetName.IsNone()
                ? FName(*FString::Printf(TEXT("OriginBatchQA_%s"),
                    *FGuid::NewGuid().ToString(EGuidFormats::Digits))) : ExplicitAssetName;
            Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(
                FixtureParentClass ? FixtureParentClass : AActor::StaticClass(), Package,
                BPName, BPTYPE_Normal, TEXT("OriginBatchHierarchyTests")));
            if (!Test.TestNotNull(TEXT("Blueprint fixture"), Blueprint.Get())) return false;
            USimpleConstructionScript* Script = SCS();
            if (!Test.TestNotNull(TEXT("SCS fixture"), Script)) return false;
            auto Add = [Script](UClass* Type, FName Name, USCS_Node* Parent,
                const FTransform& Relative) -> USCS_Node*
            {
                USCS_Node* Node = Script->CreateNode(Type, Name);
                if (!IsValid(Node)) return nullptr;
                if (Parent) Parent->AddChildNode(Node, true);
                else Script->AddNode(Node);
                if (USceneComponent* Template = Cast<USceneComponent>(Node->ComponentTemplate.Get()))
                {
                    Template->SetRelativeTransform(Relative);
                    if (!Template->GetRelativeTransform().Equals(Relative, 1.e-5) &&
                        Template->IsTemplate() && !Template->IsRegistered())
                        Template->SetRelativeTransform_Direct(Relative);
                }
                return Node;
            };
            USCS_Node* Root = Add(USceneComponent::StaticClass(), TEXT("Root"), nullptr, FTransform::Identity);
            USCS_Node* Outer = Add(UOriginPivotComponent::StaticClass(), TEXT("Outer"), Root,
                FTransform(FRotator(0, 15, 0), FVector(60, 25, 8), FVector::OneVector));
            USCS_Node* Inner = Add(UOriginPivotComponent::StaticClass(), TEXT("Inner"), Outer,
                FTransform(FRotator(0, -10, 0), FVector(-10, 22, 0), FVector::OneVector));
            USCS_Node* A = Add(USceneComponent::StaticClass(), TEXT("ChildA"), Outer,
                FTransform(FRotator(0, 20, 0), FVector(8, 50, 10), FVector::OneVector));
            USCS_Node* B = Add(USceneComponent::StaticClass(), TEXT("ChildB"), Outer,
                FTransform(FRotator(0, -35, 0), FVector(42, -13, 5), FVector::OneVector));
            USCS_Node* C = Add(USceneComponent::StaticClass(), TEXT("ChildC"), Outer,
                FTransform(FRotator(0, 5, 0), FVector(-30, 5, 0), FVector::OneVector));
            USCS_Node* SiblingA = Add(USceneComponent::StaticClass(), TEXT("SiblingA"), Root,
                FTransform(FRotator(0, 40, 0), FVector(200, 10, -5), FVector::OneVector));
            USCS_Node* SiblingB = Add(USceneComponent::StaticClass(), TEXT("SiblingB"), Root,
                FTransform(FRotator(0, -15, 0), FVector(-90, -65, 0), FVector::OneVector));
            USCS_Node* InnerChild = Add(USceneComponent::StaticClass(), TEXT("InnerChild"), Inner,
                FTransform(FRotator(0, 8, 0), FVector(19, -3, 1), FVector::OneVector));
            USCS_Node* Grandchild = Add(USceneComponent::StaticClass(), TEXT("Grandchild"), A,
                FTransform(FRotator(0, -5, 0), FVector(9, 2, 7), FVector::OneVector));
            if (!Test.TestNotNull(TEXT("Root"), Root) || !Test.TestNotNull(TEXT("Outer"), Outer) ||
                !Test.TestNotNull(TEXT("Inner"), Inner) || !Test.TestNotNull(TEXT("ChildA"), A) ||
                !Test.TestNotNull(TEXT("ChildB"), B) || !Test.TestNotNull(TEXT("ChildC"), C) ||
                !Test.TestNotNull(TEXT("SiblingA"), SiblingA) || !Test.TestNotNull(TEXT("SiblingB"), SiblingB) ||
                !Test.TestNotNull(TEXT("InnerChild"), InnerChild) || !Test.TestNotNull(TEXT("Grandchild"), Grandchild)) return false;
            FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
            return Test.TestNotNull(TEXT("Compiled fixture"), Blueprint->GeneratedClass.Get());
        }
        USimpleConstructionScript* SCS() const { return Blueprint.IsValid() ? Blueprint->SimpleConstructionScript : nullptr; }
        USCS_Node* Node(FName Name) const { return SCS() ? SCS()->FindSCSNode(Name) : nullptr; }
        UOriginPivotComponent* Pivot(FName Name = TEXT("Outer")) const
        {
            USCS_Node* N = Node(Name);
            return N ? Cast<UOriginPivotComponent>(N->ComponentTemplate.Get()) : nullptr;
        }
        FTransform Relative(FName Name) const
        {
            USCS_Node* N = Node(Name);
            const USceneComponent* T = N ? Cast<USceneComponent>(N->ComponentTemplate.Get()) : nullptr;
            return T ? T->GetRelativeTransform() : FTransform::Identity;
        }
        FTransform InRoot(FName Name) const
        {
            FTransform Total = FTransform::Identity;
            USCS_Node* Current = Node(Name);
            for (int32 Depth = 0; Depth < 128 && IsValid(Current); ++Depth)
            {
                if (Current == Node(TEXT("Root"))) break;
                Total = Total * Relative(Current->GetVariableName());
                Current = SCS()->FindParentNode(Current);
            }
            return Total;
        }
        bool IsUnder(FName Name, FName Parent) const
        {
            return SCS()->FindParentNode(Node(Name)) == Node(Parent);
        }
    };

    /** Private Undo buffer keeps automation away from user's actual editor history. */
    struct FPrivateUndo
    {
        TStrongObjectPtr<UTransactor> Original;
        TStrongObjectPtr<UTransBuffer> Isolated;
        FPrivateUndo()
        {
            if (!GEditor || !IsValid(GEditor->Trans.Get()) || GEditor->Trans->IsActive()) return;
            Original.Reset(GEditor->Trans.Get());
            UTransBuffer* Buffer = Cast<UTransBuffer>(GEditor->CreateTrans());
            if (IsValid(Buffer))
            {
                Isolated.Reset(Buffer);
                GEditor->Trans = Buffer;
            }
        }
        ~FPrivateUndo()
        {
            if (Isolated.IsValid() && GEditor) GEditor->Trans = Original.Get();
        }
        UTransBuffer* Get() const { return Isolated.Get(); }
    };
}

#define BATCH_TEST(Class, Name) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Name, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

BATCH_TEST(FOriginBatchAdopt, "Origin.Editor.Hierarchy.Blueprint.Batch.AdoptTwoAndPreserveBranches")
bool FOriginBatchAdopt::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform BeforeA = F.InRoot(TEXT("SiblingA")), BeforeB = F.InRoot(TEXT("SiblingB"));
    FText Reason;
    if (!TestTrue(TEXT("Reparent two authored siblings"),
        FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(), Names({TEXT("SiblingA"), TEXT("SiblingB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("First parent changed"), F.IsUnder(TEXT("SiblingA"), TEXT("Outer")));
    TestTrue(TEXT("Second parent changed"), F.IsUnder(TEXT("SiblingB"), TEXT("Outer")));
    TestTrue(TEXT("First placement retained"), F.InRoot(TEXT("SiblingA")).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("Second placement retained"), F.InRoot(TEXT("SiblingB")).Equals(BeforeB, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchPromote, "Origin.Editor.Hierarchy.Blueprint.Batch.PromoteTwoAndPreserveDescendant")
bool FOriginBatchPromote::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform BeforeA = F.InRoot(TEXT("ChildA"));
    const FTransform BeforeB = F.InRoot(TEXT("ChildB"));
    const FTransform Descendant = F.InRoot(TEXT("Grandchild"));
    FText Reason;
    if (!TestTrue(TEXT("Promote two authored children"),
        FOriginHierarchyAuthoring::PromoteChildren(F.Pivot(), Names({TEXT("ChildA"), TEXT("ChildB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Child A now at root"), F.IsUnder(TEXT("ChildA"), TEXT("Root")));
    TestTrue(TEXT("Child B now at root"), F.IsUnder(TEXT("ChildB"), TEXT("Root")));
    TestTrue(TEXT("Grandchild keeps parent"), F.IsUnder(TEXT("Grandchild"), TEXT("ChildA")));
    TestTrue(TEXT("A placement"), F.InRoot(TEXT("ChildA")).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("B placement"), F.InRoot(TEXT("ChildB")).Equals(BeforeB, 1.e-3));
    TestTrue(TEXT("Descendant placement"), F.InRoot(TEXT("Grandchild")).Equals(Descendant, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchDetach, "Origin.Editor.Hierarchy.Blueprint.Batch.DetachFromNestedPivotToRoot")
bool FOriginBatchDetach::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform Before = F.InRoot(TEXT("InnerChild"));
    FText Reason;
    if (!TestTrue(TEXT("Detach from nested Pivot to Blueprint root"),
        FOriginHierarchyAuthoring::DetachChildrenToRoot(F.Pivot(TEXT("Inner")), Names({TEXT("InnerChild")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Child under SCS root"), F.IsUnder(TEXT("InnerChild"), TEXT("Root")));
    TestTrue(TEXT("Full nested transform preserved"), F.InRoot(TEXT("InnerChild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchInsert, "Origin.Editor.Hierarchy.Blueprint.Batch.InsertSharedPivotPreservesBranches")
bool FOriginBatchInsert::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const int32 OriginalCount = F.SCS()->GetAllNodes().Num();
    const FTransform BeforeA = F.InRoot(TEXT("ChildA")), BeforeB = F.InRoot(TEXT("ChildB"));
    const FTransform BeforeGrandchild = F.InRoot(TEXT("Grandchild"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Insert one shared Pivot for two children"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("New pivot exists"), F.Node(Created) != nullptr);
    TestTrue(TEXT("New pivot beneath Outer"), F.IsUnder(Created, TEXT("Outer")));
    TestTrue(TEXT("Both selected children beneath new pivot"),
        F.IsUnder(TEXT("ChildA"), Created) && F.IsUnder(TEXT("ChildB"), Created));
    TestTrue(TEXT("Unselected child unaffected"), F.IsUnder(TEXT("ChildC"), TEXT("Outer")));
    TestTrue(TEXT("First placement"), F.InRoot(TEXT("ChildA")).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("Second placement"), F.InRoot(TEXT("ChildB")).Equals(BeforeB, 1.e-3));
    TestTrue(TEXT("Grandchild placement"), F.InRoot(TEXT("Grandchild")).Equals(BeforeGrandchild, 1.e-3));
    TestEqual(TEXT("Exactly one new Pivot"), F.SCS()->GetAllNodes().Num(), OriginalCount + 1);
    return true;
}

BATCH_TEST(FOriginBatchNested, "Origin.Editor.Hierarchy.Blueprint.Batch.ReparentIntoNestedPivot")
bool FOriginBatchNested::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform BeforeA = F.InRoot(TEXT("ChildA"));
    const FTransform BeforeB = F.InRoot(TEXT("ChildB"));
    const FTransform BeforeGrandchild = F.InRoot(TEXT("Grandchild"));
    FText Reason;
    if (!TestTrue(TEXT("Reparent selected components to nested Pivot"),
        FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(TEXT("Inner")),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Both now inside nested Pivot"), F.IsUnder(TEXT("ChildA"), TEXT("Inner")) &&
        F.IsUnder(TEXT("ChildB"), TEXT("Inner")));
    TestTrue(TEXT("Nested branch A"), F.InRoot(TEXT("ChildA")).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("Nested branch B"), F.InRoot(TEXT("ChildB")).Equals(BeforeB, 1.e-3));
    TestTrue(TEXT("Nested descendant"), F.InRoot(TEXT("Grandchild")).Equals(BeforeGrandchild, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchRejectDuplicates, "Origin.Editor.Hierarchy.Blueprint.Batch.DuplicateSelectionRejectsAtomically")
bool FOriginBatchRejectDuplicates::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform Before = F.InRoot(TEXT("SiblingA"));
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FText Reason;
    TestFalse(TEXT("Duplicate variable rejected"),
        FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(), Names({TEXT("SiblingA"), TEXT("SiblingA")}), Reason));
    TestTrue(TEXT("Action has useful rejection reason"), !Reason.IsEmpty());
    TestTrue(TEXT("Parent unchanged"), F.IsUnder(TEXT("SiblingA"), TEXT("Root")));
    TestTrue(TEXT("Placement unchanged"), F.InRoot(TEXT("SiblingA")).Equals(Before, 1.e-3));
    TestEqual(TEXT("Node count unchanged"), F.SCS()->GetAllNodes().Num(), Count);
    return true;
}

BATCH_TEST(FOriginBatchRejectMixed, "Origin.Editor.Hierarchy.Blueprint.Batch.MixedValidInvalidRejectsAtomically")
bool FOriginBatchRejectMixed::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform Before = F.InRoot(TEXT("SiblingA"));
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FText Reason;
    TestFalse(TEXT("Valid sibling plus non-sibling rejected as whole batch"),
        FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(),
            Names({TEXT("SiblingA"), TEXT("InnerChild")}), Reason));
    TestTrue(TEXT("First sibling parent unchanged"), F.IsUnder(TEXT("SiblingA"), TEXT("Root")));
    TestTrue(TEXT("First sibling transform unchanged"), F.InRoot(TEXT("SiblingA")).Equals(Before, 1.e-3));
    TestEqual(TEXT("No orphan SCS nodes"), F.SCS()->GetAllNodes().Num(), Count);
    return true;
}

BATCH_TEST(FOriginBatchRejectSocket, "Origin.Editor.Hierarchy.Blueprint.Batch.SocketAttachmentRejectsAtomically")
bool FOriginBatchRejectSocket::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("SiblingB"))->AttachToName = TEXT("TestSocket");
    const FTransform Before = F.InRoot(TEXT("SiblingA"));
    FText Reason;
    TestFalse(TEXT("Socket child rejects entire batch"),
        FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(),
            Names({TEXT("SiblingA"), TEXT("SiblingB")}), Reason));
    TestTrue(TEXT("First sibling unchanged"), F.IsUnder(TEXT("SiblingA"), TEXT("Root")) &&
        F.InRoot(TEXT("SiblingA")).Equals(Before, 1.e-3));
    TestTrue(TEXT("Socket metadata unchanged"), F.Node(TEXT("SiblingB"))->AttachToName == TEXT("TestSocket"));
    return true;
}

BATCH_TEST(FOriginBatchRejectAbsolute, "Origin.Editor.Hierarchy.Blueprint.Batch.AbsoluteAxisRejectsAtomically")
bool FOriginBatchRejectAbsolute::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USceneComponent* Bad = Cast<USceneComponent>(F.Node(TEXT("ChildB"))->ComponentTemplate.Get());
    if (!TestNotNull(TEXT("Template"), Bad)) return false;
    Bad->SetUsingAbsoluteLocation(true);
    const FTransform Before = F.InRoot(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Absolute child rejects whole batch"),
        FOriginHierarchyAuthoring::PromoteChildren(F.Pivot(), Names({TEXT("ChildA"), TEXT("ChildB")}), Reason));
    TestTrue(TEXT("First child unchanged"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.InRoot(TEXT("ChildA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchCompile, "Origin.Editor.Hierarchy.Blueprint.Batch.SharedPivotSurvivesCompile")
bool FOriginBatchCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform BeforeA = F.InRoot(TEXT("ChildA"));
    const FTransform BeforeB = F.InRoot(TEXT("ChildB"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Shared insertion"), FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(
        F.Pivot(), Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Pivot still authored after compile"), F.Node(Created) &&
        F.Node(Created)->ComponentTemplate->IsA<UOriginPivotComponent>());
    TestTrue(TEXT("Compiled hierarchy retains both children"), F.IsUnder(TEXT("ChildA"), Created) &&
        F.IsUnder(TEXT("ChildB"), Created));
    TestTrue(TEXT("Compiled positions A"), F.InRoot(TEXT("ChildA")).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("Compiled positions B"), F.InRoot(TEXT("ChildB")).Equals(BeforeB, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchUndoAdopt, "Origin.Editor.Hierarchy.Blueprint.Batch.Transaction.AdoptUndoRedo")
bool FOriginBatchUndoAdopt::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true)) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Isolated transaction buffer"), Buffer)) return false;
    const FTransform A = F.InRoot(TEXT("SiblingA")), B = F.InRoot(TEXT("SiblingB"));
    for (int32 Cycle = 0; Cycle < 3; ++Cycle)
    {
        FText Reason;
        if (!TestTrue(TEXT("Batch adopt"), FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(),
            Names({TEXT("SiblingA"), TEXT("SiblingB")}), Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestEqual(TEXT("Single Undo record"), Buffer->GetQueueLength(), 1)) return false;
        if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo restores both original parents"), F.IsUnder(TEXT("SiblingA"), TEXT("Root")) &&
            F.IsUnder(TEXT("SiblingB"), TEXT("Root")))) return false;
        if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo adopts both"), F.IsUnder(TEXT("SiblingA"), TEXT("Outer")) &&
            F.IsUnder(TEXT("SiblingB"), TEXT("Outer")))) return false;
        if (!TestTrue(TEXT("Redo preserves both transforms"), F.InRoot(TEXT("SiblingA")).Equals(A, 1.e-3) &&
            F.InRoot(TEXT("SiblingB")).Equals(B, 1.e-3))) return false;
        if (!TestTrue(TEXT("Undo redo"), Buffer->Undo(true))) return false;
    }
    return true;
}

BATCH_TEST(FOriginBatchUndoPromote, "Origin.Editor.Hierarchy.Blueprint.Batch.Transaction.PromoteUndoRedo")
bool FOriginBatchUndoPromote::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true)) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Isolated transaction buffer"), Buffer)) return false;
    const FTransform Before = F.InRoot(TEXT("Grandchild"));
    FText Reason;
    if (!TestTrue(TEXT("Promote both"), FOriginHierarchyAuthoring::PromoteChildren(F.Pivot(),
        Names({TEXT("ChildA"), TEXT("ChildB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("One Undo transaction"), Buffer->GetQueueLength(), 1);
    TestTrue(TEXT("Undo"), Buffer->Undo(true));
    TestTrue(TEXT("Both restored"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) && F.IsUnder(TEXT("ChildB"), TEXT("Outer")));
    TestTrue(TEXT("Redo"), Buffer->Redo());
    TestTrue(TEXT("Both promoted"), F.IsUnder(TEXT("ChildA"), TEXT("Root")) && F.IsUnder(TEXT("ChildB"), TEXT("Root")));
    TestTrue(TEXT("Grandchild unchanged"), F.InRoot(TEXT("Grandchild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchUndoDetach, "Origin.Editor.Hierarchy.Blueprint.Batch.Transaction.DetachUndoRedo")
bool FOriginBatchUndoDetach::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true)) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Isolated transaction buffer"), Buffer)) return false;
    const FTransform Before = F.InRoot(TEXT("InnerChild"));
    FText Reason;
    if (!TestTrue(TEXT("Detach nested child"), FOriginHierarchyAuthoring::DetachChildrenToRoot(
        F.Pivot(TEXT("Inner")), Names({TEXT("InnerChild")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Undo"), Buffer->Undo(true));
    TestTrue(TEXT("Parent restored"), F.IsUnder(TEXT("InnerChild"), TEXT("Inner")));
    TestTrue(TEXT("Redo"), Buffer->Redo());
    TestTrue(TEXT("Parent is root"), F.IsUnder(TEXT("InnerChild"), TEXT("Root")));
    TestTrue(TEXT("Placement retained"), F.InRoot(TEXT("InnerChild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchUndoShared, "Origin.Editor.Hierarchy.Blueprint.Batch.Transaction.InsertUndoReinsertRedo")
bool FOriginBatchUndoShared::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true)) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Isolated transaction buffer"), Buffer)) return false;
    const FTransform A = F.InRoot(TEXT("ChildA")), B = F.InRoot(TEXT("ChildB"));
    const int32 BaseCount = F.SCS()->GetAllNodes().Num();
    for (int32 Cycle = 0; Cycle < 3; ++Cycle)
    {
        FName Created;
        FText Reason;
        if (!TestTrue(TEXT("Insert shared pivot"), FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(
            F.Pivot(), Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestEqual(TEXT("One transaction"), Buffer->GetQueueLength(), 1)) return false;
        const USCS_Node* Inserted = F.Node(Created);
        if (!TestNotNull(TEXT("Inserted authored pivot"), Inserted)) return false;
        const FGuid NewGuid = Inserted->VariableGuid;
        if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo removes new node"), !F.Node(Created) &&
            F.SCS()->GetAllNodes().Num() == BaseCount)) return false;
        if (!TestTrue(TEXT("Undo restores both"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
            F.IsUnder(TEXT("ChildB"), TEXT("Outer")))) return false;
        if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
        const USCS_Node* Redone = F.Node(Created);
        if (!TestNotNull(TEXT("Redone pivot"), Redone)) return false;
        if (!TestTrue(TEXT("Preserved GUID"), Redone->VariableGuid == NewGuid)) return false;
        if (!TestTrue(TEXT("Redo preserves topology"), F.IsUnder(TEXT("ChildA"), Created) &&
            F.IsUnder(TEXT("ChildB"), Created))) return false;
        if (!TestTrue(TEXT("Redo preserves transforms"), F.InRoot(TEXT("ChildA")).Equals(A, 1.e-3) &&
            F.InRoot(TEXT("ChildB")).Equals(B, 1.e-3))) return false;
        if (!TestTrue(TEXT("Undo after redo"), Buffer->Undo(true))) return false;
    }
    return true;
}


BATCH_TEST(FOriginBatchNativeAttachedRoot, "Origin.Editor.Hierarchy.Blueprint.Batch.NativeAttachedRootDetachPreservesBranch")
bool FOriginBatchNativeAttachedRoot::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USCS_Node* Root = F.Node(TEXT("Root"));
    if (!TestNotNull(TEXT("Authored scene root"), Root)) return false;
    // The authored root is bound to a native parent; only its children move.
    Root->bIsParentComponentNative = true;
    Root->ParentComponentOrVariableName = TEXT("NativeSceneRoot");
    Root->AttachToName = TEXT("RootSocket");
    const FTransform Before = F.InRoot(TEXT("InnerChild"));
    const FTransform OuterBefore = F.InRoot(TEXT("Outer"));
    FText Reason;
    if (!TestTrue(TEXT("Detach within authored subtree beneath native/socket root"),
        FOriginHierarchyAuthoring::DetachChildrenToRoot(F.Pivot(TEXT("Inner")),
            Names({TEXT("InnerChild")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Child now directly under unchanged authored root"),
        F.IsUnder(TEXT("InnerChild"), TEXT("Root")));
    TestTrue(TEXT("Child transform preserved in common root frame"),
        F.InRoot(TEXT("InnerChild")).Equals(Before, 1.e-3));
    TestTrue(TEXT("Unselected pivot remains stationary"), F.InRoot(TEXT("Outer")).Equals(OuterBefore, 1.e-3));
    TestTrue(TEXT("Native parent and root socket metadata not touched"),
        Root->bIsParentComponentNative && Root->ParentComponentOrVariableName == TEXT("NativeSceneRoot") &&
        Root->AttachToName == TEXT("RootSocket"));
    return true;
}

BATCH_TEST(FOriginBatchRejectEmpty, "Origin.Editor.Hierarchy.Blueprint.Batch.EmptySelectionRejectsAtomically")
bool FOriginBatchRejectEmpty::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FText Reason;
    FName NewPivot = NAME_None;
    TestFalse(TEXT("Empty reparent"), FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(), {}, Reason));
    TestFalse(TEXT("Empty promote"), FOriginHierarchyAuthoring::PromoteChildren(F.Pivot(), {}, Reason));
    TestFalse(TEXT("Empty detach"), FOriginHierarchyAuthoring::DetachChildrenToRoot(F.Pivot(), {}, Reason));
    TestFalse(TEXT("Empty shared insertion"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(), {}, NewPivot, Reason));
    TestEqual(TEXT("AllNodes unchanged"), F.SCS()->GetAllNodes().Num(), Count);
    TestTrue(TEXT("Children unchanged"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.IsUnder(TEXT("ChildB"), TEXT("Outer")));
    return true;
}

BATCH_TEST(FOriginBatchRejectUnsafeScale, "Origin.Editor.Hierarchy.Blueprint.Batch.NonuniformScaleRejectsAtomically")
bool FOriginBatchRejectUnsafeScale::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USceneComponent* Outer = Cast<USceneComponent>(F.Node(TEXT("Outer"))->ComponentTemplate.Get());
    if (!TestNotNull(TEXT("Outer template"), Outer)) return false;
    FTransform T = Outer->GetRelativeTransform();
    T.SetScale3D(FVector(1.0, 2.0, 1.0));
    Outer->SetRelativeTransform_Direct(T);
    const FTransform Before = F.InRoot(TEXT("SiblingA"));
    FText Reason;
    TestFalse(TEXT("Reject skew-prone nonuniform scale"),
        FOriginHierarchyAuthoring::AdoptSiblings(F.Pivot(), Names({TEXT("SiblingA")}), Reason));
    TestTrue(TEXT("Original parent retained"), F.IsUnder(TEXT("SiblingA"), TEXT("Root")));
    TestTrue(TEXT("Placement retained"), F.InRoot(TEXT("SiblingA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginBatchRejectSharedMixed, "Origin.Editor.Hierarchy.Blueprint.Batch.SharedInsertInvalidSelectionAtomic")
bool FOriginBatchRejectSharedMixed::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const int32 Count = F.SCS()->GetAllNodes().Num();
    const FTransform A = F.InRoot(TEXT("ChildA"));
    FName Created;
    FText Reason;
    TestFalse(TEXT("Reject one valid + one nondirect child"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("InnerChild")}), Created, Reason));
    TestTrue(TEXT("No pivot reported"), Created.IsNone());
    TestEqual(TEXT("No new SCS nodes"), F.SCS()->GetAllNodes().Num(), Count);
    TestTrue(TEXT("Valid child parent untouched"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")));
    TestTrue(TEXT("Valid child transform untouched"), F.InRoot(TEXT("ChildA")).Equals(A, 1.e-3));
    return true;
}


BATCH_TEST(FOriginBatchRejectPivotSocket, "Origin.Editor.Hierarchy.Blueprint.Batch.PivotSocketRejectsAtomically")
bool FOriginBatchRejectPivotSocket::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("Outer"))->AttachToName = TEXT("BoneSocket");
    const FTransform Before = F.Relative(TEXT("ChildA"));
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FText Reason;
    TestFalse(TEXT("Socket-attached Pivot cannot reparent authored children"),
        FOriginHierarchyAuthoring::PromoteChildren(F.Pivot(), Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("Original parent remains"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")));
    TestTrue(TEXT("Original authored relative remains"), F.Relative(TEXT("ChildA")).Equals(Before, 1.e-3));
    TestEqual(TEXT("No new nodes"), F.SCS()->GetAllNodes().Num(), Count);
    return true;
}


// Alpha 5.6: cross-pivot transfers, inherited/socket guards, and transaction parity.
namespace
{
    USCS_Node* AddTransferTarget(FBatchFixture& F, const TCHAR* Variable, FName ParentName)
    {
        USCS_Node* Node = F.SCS()->CreateNode(UOriginPivotComponent::StaticClass(), FName(Variable));
        USCS_Node* Parent = F.Node(ParentName);
        if (!IsValid(Node) || !IsValid(Parent)) return nullptr;
        Parent->AddChildNode(Node, true);
        const FTransform Relative(FRotator(0, 32, 0), FVector(80, -35, 14), FVector::OneVector);
        if (USceneComponent* Template = Cast<USceneComponent>(Node->ComponentTemplate.Get()))
        {
            Template->SetRelativeTransform(Relative);
            if (!Template->GetRelativeTransform().Equals(Relative, 1.e-5) &&
                Template->IsTemplate() && !Template->IsRegistered())
                Template->SetRelativeTransform_Direct(Relative);
        }
        return Node;
    }
}

BATCH_TEST(FOriginTransferTwoBranches, "Origin.Editor.Hierarchy.Blueprint.Transfer.TwoBranchesPreserveWorld")
bool FOriginTransferTwoBranches::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this) || !TestNotNull(TEXT("Second Origin Pivot"),
        AddTransferTarget(F, TEXT("OtherPivot"), TEXT("Root")))) return false;
    const FTransform A = F.InRoot(TEXT("ChildA"));
    const FTransform B = F.InRoot(TEXT("ChildB"));
    const FTransform Grandchild = F.InRoot(TEXT("Grandchild"));
    const FTransform C = F.InRoot(TEXT("ChildC"));
    FText Reason;
    if (!TestTrue(TEXT("Transfer two sibling branches"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("OtherPivot"),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Both moved to destination"),
        F.IsUnder(TEXT("ChildA"), TEXT("OtherPivot")) && F.IsUnder(TEXT("ChildB"), TEXT("OtherPivot")));
    TestTrue(TEXT("Unselected remains"), F.IsUnder(TEXT("ChildC"), TEXT("Outer")));
    TestTrue(TEXT("A location preserved"), F.InRoot(TEXT("ChildA")).Equals(A, 1.e-3));
    TestTrue(TEXT("B location preserved"), F.InRoot(TEXT("ChildB")).Equals(B, 1.e-3));
    TestTrue(TEXT("Grandchild subtree preserved"), F.IsUnder(TEXT("Grandchild"), TEXT("ChildA")) &&
        F.InRoot(TEXT("Grandchild")).Equals(Grandchild, 1.e-3));
    TestTrue(TEXT("Unselected unchanged"), F.InRoot(TEXT("ChildC")).Equals(C, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferNestedDown, "Origin.Editor.Hierarchy.Blueprint.Transfer.IntoNestedPivot")
bool FOriginTransferNestedDown::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform Before = F.InRoot(TEXT("Grandchild"));
    FText Reason;
    if (!TestTrue(TEXT("Transfer branch to nested pivot"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("New parent is nested pivot"), F.IsUnder(TEXT("ChildA"), TEXT("Inner")));
    TestTrue(TEXT("Descendant follows without moving"), F.InRoot(TEXT("Grandchild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferNestedUp, "Origin.Editor.Hierarchy.Blueprint.Transfer.OutOfNestedPivot")
bool FOriginTransferNestedUp::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform Before = F.InRoot(TEXT("InnerChild"));
    FText Reason;
    if (!TestTrue(TEXT("Transfer out of nested pivot"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(TEXT("Inner")), TEXT("Outer"),
            Names({TEXT("InnerChild")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Now child of outer pivot"), F.IsUnder(TEXT("InnerChild"), TEXT("Outer")));
    TestTrue(TEXT("Location retained"), F.InRoot(TEXT("InnerChild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferTargets, "Origin.Editor.Hierarchy.Blueprint.Transfer.DiscoveryOnlyCompatibleTargets")
bool FOriginTransferTargets::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    if (!TestNotNull(TEXT("Second pivot"), AddTransferTarget(F, TEXT("OtherPivot"), TEXT("Root")))) return false;
    TArray<FName> Targets;
    FOriginHierarchyAuthoring::ListTransferTargets(F.Pivot(), Targets);
    TestTrue(TEXT("Inner pivot available"), Targets.Contains(TEXT("Inner")));
    TestTrue(TEXT("Separate pivot available"), Targets.Contains(TEXT("OtherPivot")));
    TestFalse(TEXT("Never include self"), Targets.Contains(TEXT("Outer")));
    return true;
}

BATCH_TEST(FOriginTransferCycle, "Origin.Editor.Hierarchy.Blueprint.Transfer.DescendantCycleRejectsAtomically")
bool FOriginTransferCycle::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this) || !TestNotNull(TEXT("Pivot inside selected branch"),
        AddTransferTarget(F, TEXT("BranchPivot"), TEXT("ChildA")))) return false;
    const FTransform Before = F.InRoot(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Reject cycle"), FOriginHierarchyAuthoring::TransferChildrenToPivot(
        F.Pivot(), TEXT("BranchPivot"), Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("Useful error"), !Reason.IsEmpty());
    TestTrue(TEXT("Parent unchanged"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")));
    TestTrue(TEXT("Transform unchanged"), F.InRoot(TEXT("ChildA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferMixed, "Origin.Editor.Hierarchy.Blueprint.Transfer.MixedInvalidBatchIsAtomic")
bool FOriginTransferMixed::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    const FTransform A = F.InRoot(TEXT("ChildA"));
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FText Reason;
    TestFalse(TEXT("Reject mixed valid + nondirect child"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA"), TEXT("SiblingA")}), Reason));
    TestTrue(TEXT("Non-empty reason"), !Reason.IsEmpty());
    TestTrue(TEXT("Correct parent"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")));
    TestTrue(TEXT("Transform retained"), F.InRoot(TEXT("ChildA")).Equals(A, 1.e-3));
    TestEqual(TEXT("Node count retained"), F.SCS()->GetAllNodes().Num(), Count);
    return true;
}

BATCH_TEST(FOriginTransferSocket, "Origin.Editor.Hierarchy.Blueprint.Transfer.SocketPathRejectsAtomically")
bool FOriginTransferSocket::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("Inner"))->AttachToName = TEXT("SomeBoneSocket");
    const FTransform A = F.Relative(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Reject destination attached to socket"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("Source unchanged"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.Relative(TEXT("ChildA")).Equals(A, 1.e-3));
    TestTrue(TEXT("Reason present"), !Reason.IsEmpty());
    return true;
}

BATCH_TEST(FOriginTransferNativeAttachedRoot, "Origin.Editor.Hierarchy.Blueprint.Transfer.NativeAttachedRootPreservesBranches")
bool FOriginTransferNativeAttachedRoot::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USCS_Node* Root = F.Node(TEXT("Root"));
    if (!TestNotNull(TEXT("Authored scene root"), Root)) return false;
    Root->bIsParentComponentNative = true;
    Root->ParentComponentOrVariableName = TEXT("NativeSceneRoot");
    Root->ParentComponentOwnerClassName = TEXT("NativeFixtureClass");
    Root->AttachToName = TEXT("NativeSocket");
    const FTransform Before = F.InRoot(TEXT("Grandchild"));
    const FTransform BeforeC = F.InRoot(TEXT("ChildC"));
    FText Reason;
    if (!TestTrue(TEXT("Move only descendants sharing unchanged authored root"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Branch moved under nested pivot"), F.IsUnder(TEXT("ChildA"), TEXT("Inner")));
    TestTrue(TEXT("Descendant retains placement relative to authored root"),
        F.InRoot(TEXT("Grandchild")).Equals(Before, 1.e-3));
    TestTrue(TEXT("Unselected branch untouched"), F.InRoot(TEXT("ChildC")).Equals(BeforeC, 1.e-3));
    TestTrue(TEXT("External root attachment unchanged"), Root->bIsParentComponentNative &&
        Root->ParentComponentOrVariableName == TEXT("NativeSceneRoot") &&
        Root->ParentComponentOwnerClassName == TEXT("NativeFixtureClass") &&
        Root->AttachToName == TEXT("NativeSocket"));
    return true;
}

BATCH_TEST(FOriginTransferNativeRootSocketBelow, "Origin.Editor.Hierarchy.Blueprint.Transfer.NativeRootDoesNotAllowChildSocketPath")
bool FOriginTransferNativeRootSocketBelow::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USCS_Node* Root = F.Node(TEXT("Root"));
    Root->bIsParentComponentNative = true;
    Root->ParentComponentOrVariableName = TEXT("NativeParent");
    F.Node(TEXT("Inner"))->AttachToName = TEXT("SocketBelowRoot");
    const FTransform Before = F.InRoot(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Socket-dependent frame below the shared root remains unsupported"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("Failed operation is atomic"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.InRoot(TEXT("ChildA")).Equals(Before, 1.e-3));
    TestTrue(TEXT("Original native parent metadata remains"),
        Root->ParentComponentOrVariableName == TEXT("NativeParent"));
    return true;
}

BATCH_TEST(FOriginTransferScale, "Origin.Editor.Hierarchy.Blueprint.Transfer.NonuniformScaleRejectsAtomically")
bool FOriginTransferScale::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USceneComponent* Target = Cast<USceneComponent>(F.Node(TEXT("Inner"))->ComponentTemplate.Get());
    if (!TestNotNull(TEXT("Inner authored template"), Target)) return false;
    FTransform Changed = Target->GetRelativeTransform();
    Changed.SetScale3D(FVector(2, 1, 1));
    Target->SetRelativeTransform_Direct(Changed);
    const FTransform Before = F.Relative(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Unsafe nonuniform destination"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("Source unchanged"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.Relative(TEXT("ChildA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferCompile, "Origin.Editor.Hierarchy.Blueprint.Transfer.CompileRetainsTransfer")
bool FOriginTransferCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this) || !TestNotNull(TEXT("Second pivot"),
        AddTransferTarget(F, TEXT("OtherPivot"), TEXT("Root")))) return false;
    const FTransform Before = F.InRoot(TEXT("Grandchild"));
    FText Reason;
    if (!TestTrue(TEXT("Transfer before compile"), FOriginHierarchyAuthoring::TransferChildrenToPivot(
        F.Pivot(), TEXT("OtherPivot"), Names({TEXT("ChildA")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Authored destination survives compile"),
        F.Node(TEXT("OtherPivot")) && F.Node(TEXT("OtherPivot"))->ComponentTemplate->IsA<UOriginPivotComponent>());
    TestTrue(TEXT("Authored transfer survives compile"), F.IsUnder(TEXT("ChildA"), TEXT("OtherPivot")));
    TestTrue(TEXT("Descendant transform survives"), F.InRoot(TEXT("Grandchild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferTargetFilterCycle, "Origin.Editor.Hierarchy.Blueprint.Transfer.SelectionTargetsHideDescendantCycles")
bool FOriginTransferTargetFilterCycle::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this) || !TestNotNull(TEXT("Pivot within selected branch"),
        AddTransferTarget(F, TEXT("BranchPivot"), TEXT("ChildA")))) return false;
    const FTransform Before = F.InRoot(TEXT("Grandchild"));
    TArray<FName> Candidates;
    FOriginHierarchyAuthoring::ListTransferTargetsForChildren(F.Pivot(),
        Names({TEXT("ChildA")}), Candidates);
    TestFalse(TEXT("Selected branch may not be reparented below its own descendant"),
        Candidates.Contains(TEXT("BranchPivot")));
    TestTrue(TEXT("Non-cyclic nested pivot remains eligible"), Candidates.Contains(TEXT("Inner")));
    TestTrue(TEXT("Discovery must not move nodes"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.InRoot(TEXT("Grandchild")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferTargetFilterScale, "Origin.Editor.Hierarchy.Blueprint.Transfer.SelectionTargetsHideUnsafeScale")
bool FOriginTransferTargetFilterScale::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this) || !TestNotNull(TEXT("Second Pivot"),
        AddTransferTarget(F, TEXT("OtherPivot"), TEXT("Root")))) return false;
    USceneComponent* Unsafe = Cast<USceneComponent>(F.Node(TEXT("Inner"))->ComponentTemplate.Get());
    if (!TestNotNull(TEXT("Inner template"), Unsafe)) return false;
    FTransform Bad = Unsafe->GetRelativeTransform();
    Bad.SetScale3D(FVector(2, 1, 1));
    Unsafe->SetRelativeTransform_Direct(Bad);
    const FTransform Before = F.InRoot(TEXT("ChildA"));
    TArray<FName> Candidates;
    FOriginHierarchyAuthoring::ListTransferTargetsForChildren(F.Pivot(),
        Names({TEXT("ChildA")}), Candidates);
    TestFalse(TEXT("Unsafe scale destination hidden"), Candidates.Contains(TEXT("Inner")));
    TestTrue(TEXT("Safe sibling pivot remains visible"), Candidates.Contains(TEXT("OtherPivot")));
    TestTrue(TEXT("No mutation in filtered discovery"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.InRoot(TEXT("ChildA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferTargetFilterSelection, "Origin.Editor.Hierarchy.Blueprint.Transfer.SelectionTargetsRejectInvalidChild")
bool FOriginTransferTargetFilterSelection::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    TArray<FName> Candidates;
    FOriginHierarchyAuthoring::ListTransferTargetsForChildren(F.Pivot(), {}, Candidates);
    TestTrue(TEXT("Without selection, valid target discovery is unchanged"), Candidates.Contains(TEXT("Inner")));
    FOriginHierarchyAuthoring::ListTransferTargetsForChildren(F.Pivot(),
        Names({TEXT("ChildA"), TEXT("SiblingA")}), Candidates);
    TestEqual(TEXT("Mixed invalid selection has no eligible destination"), Candidates.Num(), 0);
    TestTrue(TEXT("No partial movement"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.IsUnder(TEXT("SiblingA"), TEXT("Root")));
    return true;
}

BATCH_TEST(FOriginTransferRootDomains, "Origin.Editor.Hierarchy.Blueprint.Transfer.SeparateAuthoredRootsReject")
bool FOriginTransferRootDomains::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    // A second authored SCS root is a distinct frame. Native/inherited
    // parents may differ, so cross-root moves remain unsafe and rejected.
    USCS_Node* AnotherRoot = F.SCS()->CreateNode(USceneComponent::StaticClass(), TEXT("OtherRoot"));
    F.SCS()->AddNode(AnotherRoot);
    USCS_Node* DifferentPivot = F.SCS()->CreateNode(UOriginPivotComponent::StaticClass(), TEXT("OtherRootPivot"));
    AnotherRoot->AddChildNode(DifferentPivot, true);
    const FTransform Before = F.InRoot(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Transfer must reject different root frames"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("OtherRootPivot"),
            Names({TEXT("ChildA")}), Reason));
    TArray<FName> Candidates;
    FOriginHierarchyAuthoring::ListTransferTargetsForChildren(F.Pivot(),
        Names({TEXT("ChildA")}), Candidates);
    TestFalse(TEXT("Cross-root destination excluded"), Candidates.Contains(TEXT("OtherRootPivot")));
    TestTrue(TEXT("Authored source remains unchanged"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.InRoot(TEXT("ChildA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginTransferUndo, "Origin.Editor.Hierarchy.Blueprint.Transfer.Transaction.RepeatedUndoRedo")
bool FOriginTransferUndo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true) || !TestNotNull(TEXT("Second pivot"),
        AddTransferTarget(F, TEXT("OtherPivot"), TEXT("Root")))) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Private Undo buffer"), Buffer)) return false;
    const FTransform Original = F.InRoot(TEXT("Grandchild"));
    for (int32 Cycle = 0; Cycle < 3; ++Cycle)
    {
        FText Reason;
        if (!TestTrue(TEXT("Transfer a branch"), FOriginHierarchyAuthoring::TransferChildrenToPivot(
            F.Pivot(), TEXT("OtherPivot"), Names({TEXT("ChildA")}), Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestEqual(TEXT("One transaction per transfer"), Buffer->GetQueueLength(), 1)) return false;
        if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
        if (!TestTrue(TEXT("Undo restores source"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")))) return false;
        if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
        if (!TestTrue(TEXT("Redo restores destination"), F.IsUnder(TEXT("ChildA"), TEXT("OtherPivot")))) return false;
        if (!TestTrue(TEXT("Descendant preserved"),
            F.InRoot(TEXT("Grandchild")).Equals(Original, 1.e-3))) return false;
        if (!TestTrue(TEXT("Undo after redo"), Buffer->Undo(true))) return false;
    }
    return true;
}

// Alpha 5.8: A socket-aware INSERT is an exact parent-socket handoff, not a
// generic socket reparent. Every assertion checks authored SCS attachment
// metadata AND the complete local child-to-socket transform before/after.
// This remains safe for animated bones because the original parent and socket
// name are identical; no sampling of an animation frame is required.
BATCH_TEST(FOriginSocketInsertOne, "Origin.Editor.Hierarchy.Blueprint.Socket.SingleHandoffPreservesBranch")
bool FOriginSocketInsertOne::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    const FTransform BeforeChild = F.Relative(TEXT("ChildA"));
    const FTransform BeforeGrandchild = F.Relative(TEXT("Grandchild")) * BeforeChild;
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Insert pivot into a socket-attached branch"),
        FOriginHierarchyAuthoring::InsertPivotAboveChild(F.Pivot(), TEXT("ChildA"), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("New pivot remains below the SAME source parent"), F.IsUnder(Created, TEXT("Outer")));
    TestTrue(TEXT("Existing child is below the new pivot"), F.IsUnder(TEXT("ChildA"), Created));
    TestTrue(TEXT("Socket name transferred exactly"), F.Node(Created)->AttachToName == TEXT("GripSocket"));
    TestTrue(TEXT("Original child no longer owns the socket"), F.Node(TEXT("ChildA"))->AttachToName.IsNone());
    TestTrue(TEXT("Placement in unchanged socket frame"),
        (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(BeforeChild, 1.e-3));
    TestTrue(TEXT("Descendant placement in unchanged socket frame"),
        (F.Relative(TEXT("Grandchild")) * F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(BeforeGrandchild, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketInsertOneCompile, "Origin.Editor.Hierarchy.Blueprint.Socket.SingleHandoffSurvivesCompile")
bool FOriginSocketInsertOneCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    const FTransform Before = F.Relative(TEXT("ChildA"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Socket insertion"), FOriginHierarchyAuthoring::InsertPivotAboveChild(
        F.Pivot(), TEXT("ChildA"), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Inserted pivot retained"), IsValid(F.Node(Created)) &&
        F.Node(Created)->ComponentTemplate->IsA<UOriginPivotComponent>());
    TestTrue(TEXT("Socket handoff retained by Blueprint compilation"),
        F.IsUnder(TEXT("ChildA"), Created) && F.Node(Created)->AttachToName == TEXT("GripSocket") &&
        F.Node(TEXT("ChildA"))->AttachToName.IsNone());
    TestTrue(TEXT("Socket-space placement retained by Blueprint compilation"),
        (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketInsertTwo, "Origin.Editor.Hierarchy.Blueprint.Socket.SharedSameSocketPreservesBranches")
bool FOriginSocketInsertTwo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    F.Node(TEXT("ChildB"))->AttachToName = TEXT("GripSocket");
    const FTransform BeforeA = F.Relative(TEXT("ChildA"));
    const FTransform BeforeB = F.Relative(TEXT("ChildB"));
    const FTransform BeforeGrandchild = F.Relative(TEXT("Grandchild")) * BeforeA;
    const int32 OldCount = F.SCS()->GetAllNodes().Num();
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group children on the same socket"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Exactly one Pivot added"), F.SCS()->GetAllNodes().Num(), OldCount + 1);
    TestTrue(TEXT("Common socket moved to new Pivot"),
        F.IsUnder(Created, TEXT("Outer")) && F.Node(Created)->AttachToName == TEXT("GripSocket"));
    TestTrue(TEXT("Both children now under Pivot with sockets cleared"),
        F.IsUnder(TEXT("ChildA"), Created) && F.IsUnder(TEXT("ChildB"), Created) &&
        F.Node(TEXT("ChildA"))->AttachToName.IsNone() && F.Node(TEXT("ChildB"))->AttachToName.IsNone());
    TestTrue(TEXT("A in original socket frame"),
        (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("B in original socket frame"),
        (F.Relative(TEXT("ChildB")) * F.Relative(Created)).Equals(BeforeB, 1.e-3));
    TestTrue(TEXT("Grandchild in original socket frame"),
        (F.Relative(TEXT("Grandchild")) * F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(BeforeGrandchild, 1.e-3));
    TestTrue(TEXT("Unselected child stays under original Pivot"), F.IsUnder(TEXT("ChildC"), TEXT("Outer")));
    return true;
}

BATCH_TEST(FOriginSocketInsertThree, "Origin.Editor.Hierarchy.Blueprint.Socket.SharedThreeChildrenPreserve")
bool FOriginSocketInsertThree::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    for (const FName Name : Names({TEXT("ChildA"), TEXT("ChildB"), TEXT("ChildC")}))
        F.Node(Name)->AttachToName = TEXT("GripSocket");
    TMap<FName, FTransform> Original;
    for (const FName Name : Names({TEXT("ChildA"), TEXT("ChildB"), TEXT("ChildC")}))
        Original.Add(Name, F.Relative(Name));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group three children under a socket Pivot"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildB"), TEXT("ChildC")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Same socket on new Pivot"), F.Node(Created)->AttachToName == TEXT("GripSocket"));
    for (const FName Name : Names({TEXT("ChildA"), TEXT("ChildB"), TEXT("ChildC")}))
        TestTrue(*FString::Printf(TEXT("%s unchanged in socket frame"), *Name.ToString()),
            F.IsUnder(Name, Created) && F.Node(Name)->AttachToName.IsNone() &&
            (F.Relative(Name) * F.Relative(Created)).Equals(Original[Name], 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketDifferentSockets, "Origin.Editor.Hierarchy.Blueprint.Socket.RejectDifferentSocketsAtomically")
bool FOriginSocketDifferentSockets::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    F.Node(TEXT("ChildB"))->AttachToName = TEXT("MuzzleSocket");
    const int32 Count = F.SCS()->GetAllNodes().Num();
    const FTransform Before = F.Relative(TEXT("ChildA"));
    FName Created;
    FText Reason;
    TestFalse(TEXT("Cannot combine different animated socket frames"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason));
    TestTrue(TEXT("Useful reason"), !Reason.IsEmpty());
    TestTrue(TEXT("No new Pivot"), Created.IsNone() && F.SCS()->GetAllNodes().Num() == Count);
    TestTrue(TEXT("Original parents and socket names retained"),
        F.IsUnder(TEXT("ChildA"), TEXT("Outer")) && F.IsUnder(TEXT("ChildB"), TEXT("Outer")) &&
        F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket") &&
        F.Node(TEXT("ChildB"))->AttachToName == TEXT("MuzzleSocket"));
    TestTrue(TEXT("Original transform retained"), F.Relative(TEXT("ChildA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketMixedNone, "Origin.Editor.Hierarchy.Blueprint.Socket.RejectSocketAndUnattachedMix")
bool FOriginSocketMixedNone::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    FName Created;
    FText Reason;
    TestFalse(TEXT("Socket and ordinary parent frames cannot be merged"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason));
    TestTrue(TEXT("No topology changed"), Created.IsNone() && F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.IsUnder(TEXT("ChildB"), TEXT("Outer")) && F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket"));
    return true;
}

BATCH_TEST(FOriginSocketInvalidBatch, "Origin.Editor.Hierarchy.Blueprint.Socket.RejectValidAndInvalidSelection")
bool FOriginSocketInvalidBatch::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FName Created;
    FText Reason;
    TestFalse(TEXT("Mixed direct and non-direct child selection rejected"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("SiblingA")}), Created, Reason));
    TestTrue(TEXT("No inserted node or lost socket"),
        Created.IsNone() && F.SCS()->GetAllNodes().Num() == Count &&
        F.IsUnder(TEXT("ChildA"), TEXT("Outer")) && F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket"));
    return true;
}

BATCH_TEST(FOriginSocketDuplicateBatch, "Origin.Editor.Hierarchy.Blueprint.Socket.RejectDuplicateSelection")
bool FOriginSocketDuplicateBatch::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    FName Created;
    FText Reason;
    TestFalse(TEXT("Same authored node selected twice rejected"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildA")}), Created, Reason));
    TestTrue(TEXT("Socket and parent retained"),
        Created.IsNone() && F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket"));
    return true;
}

BATCH_TEST(FOriginSocketSharedCompile, "Origin.Editor.Hierarchy.Blueprint.Socket.SharedHandoffSurvivesCompile")
bool FOriginSocketSharedCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    F.Node(TEXT("ChildB"))->AttachToName = TEXT("GripSocket");
    const FTransform BeforeA = F.Relative(TEXT("ChildA"));
    const FTransform BeforeB = F.Relative(TEXT("ChildB"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Socket grouping"), FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(
        F.Pivot(), Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Compiled Pivot owns socket"), IsValid(F.Node(Created)) &&
        F.Node(Created)->AttachToName == TEXT("GripSocket"));
    TestTrue(TEXT("Compiled children remain under inserted Pivot, with no sockets"),
        F.IsUnder(TEXT("ChildA"), Created) && F.IsUnder(TEXT("ChildB"), Created) &&
        F.Node(TEXT("ChildA"))->AttachToName.IsNone() && F.Node(TEXT("ChildB"))->AttachToName.IsNone());
    TestTrue(TEXT("A frame"), (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("B frame"), (F.Relative(TEXT("ChildB")) * F.Relative(Created)).Equals(BeforeB, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketList, "Origin.Editor.Hierarchy.Blueprint.Socket.DiscoveryOnlyInsertableChildren")
bool FOriginSocketList::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    TArray<FName> Siblings, Children, Insertable;
    FOriginHierarchyAuthoring::ListSiblingsAndChildren(F.Pivot(), Siblings, Children);
    FOriginHierarchyAuthoring::ListInsertableChildren(F.Pivot(), Insertable);
    TestFalse(TEXT("Promote/transfer selection excludes socket"), Children.Contains(TEXT("ChildA")));
    TestTrue(TEXT("Insert selection includes socket"), Insertable.Contains(TEXT("ChildA")));
    TestTrue(TEXT("Normal child remains eligible"), Children.Contains(TEXT("ChildB")) &&
        Insertable.Contains(TEXT("ChildB")));
    TestTrue(TEXT("Discovery never edits socket"), F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket") &&
        F.IsUnder(TEXT("ChildA"), TEXT("Outer")));
    return true;
}

BATCH_TEST(FOriginSocketReparentReject, "Origin.Editor.Hierarchy.Blueprint.Socket.PromoteDetachAndTransferStayProtected")
bool FOriginSocketReparentReject::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    FText Reason;
    TestFalse(TEXT("Promotion cannot reinterpret socket"),
        FOriginHierarchyAuthoring::PromoteChild(F.Pivot(), TEXT("ChildA"), Reason));
    TestFalse(TEXT("Detach cannot reinterpret socket"),
        FOriginHierarchyAuthoring::DetachChildrenToRoot(F.Pivot(), Names({TEXT("ChildA")}), Reason));
    TestFalse(TEXT("Transfer cannot reinterpret socket"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("All rejections preserve state"),
        F.IsUnder(TEXT("ChildA"), TEXT("Outer")) && F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket"));
    return true;
}

BATCH_TEST(FOriginSocketAbsoluteReject, "Origin.Editor.Hierarchy.Blueprint.Socket.RejectAbsoluteSocketChild")
bool FOriginSocketAbsoluteReject::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USceneComponent* Template = Cast<USceneComponent>(F.Node(TEXT("ChildA"))->ComponentTemplate.Get());
    if (!TestNotNull(TEXT("Child template"), Template)) return false;
    Template->SetUsingAbsoluteLocation(true);
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    FName Created;
    FText Reason;
    TestFalse(TEXT("Absolute location makes handoff unsafe"),
        FOriginHierarchyAuthoring::InsertPivotAboveChild(F.Pivot(), TEXT("ChildA"), Created, Reason));
    TestTrue(TEXT("Original parent and socket unchanged"),
        Created.IsNone() && F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket"));
    return true;
}

BATCH_TEST(FOriginSocketOneUndo, "Origin.Editor.Hierarchy.Blueprint.Socket.Transaction.SingleUndoRedoRestoresSocket")
bool FOriginSocketOneUndo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    const FTransform Before = F.Relative(TEXT("ChildA"));
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Private Undo buffer"), Buffer)) return false;
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Transactional socket insertion"),
        FOriginHierarchyAuthoring::InsertPivotAboveChild(F.Pivot(), TEXT("ChildA"), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("One Undo entry"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
    TestTrue(TEXT("Undo restores original parent/socket"),
        F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket") &&
        F.Relative(TEXT("ChildA")).Equals(Before, 1.e-3));
    if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
    TestTrue(TEXT("Redo restores handoff"),
        F.IsUnder(TEXT("ChildA"), Created) && F.Node(Created)->AttachToName == TEXT("GripSocket") &&
        F.Node(TEXT("ChildA"))->AttachToName.IsNone() &&
        (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketSharedUndo, "Origin.Editor.Hierarchy.Blueprint.Socket.Transaction.SharedUndoRedoRestoresSockets")
bool FOriginSocketSharedUndo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this, true)) return false;
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    F.Node(TEXT("ChildB"))->AttachToName = TEXT("GripSocket");
    const FTransform BeforeA = F.Relative(TEXT("ChildA"));
    const FTransform BeforeB = F.Relative(TEXT("ChildB"));
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Private Undo buffer"), Buffer)) return false;
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Transactional shared socket insertion"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(F.Pivot(),
            Names({TEXT("ChildA"), TEXT("ChildB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Single Undo entry"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
    TestTrue(TEXT("Undo restores original socket on both"),
        F.IsUnder(TEXT("ChildA"), TEXT("Outer")) && F.IsUnder(TEXT("ChildB"), TEXT("Outer")) &&
        F.Node(TEXT("ChildA"))->AttachToName == TEXT("GripSocket") &&
        F.Node(TEXT("ChildB"))->AttachToName == TEXT("GripSocket") &&
        F.Relative(TEXT("ChildA")).Equals(BeforeA, 1.e-3) &&
        F.Relative(TEXT("ChildB")).Equals(BeforeB, 1.e-3));
    if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
    TestTrue(TEXT("Redo restores shared socket"),
        F.IsUnder(TEXT("ChildA"), Created) && F.IsUnder(TEXT("ChildB"), Created) &&
        F.Node(Created)->AttachToName == TEXT("GripSocket") &&
        F.Node(TEXT("ChildA"))->AttachToName.IsNone() && F.Node(TEXT("ChildB"))->AttachToName.IsNone() &&
        (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(BeforeA, 1.e-3) &&
        (F.Relative(TEXT("ChildB")) * F.Relative(Created)).Equals(BeforeB, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketNativeParent, "Origin.Editor.Hierarchy.Blueprint.Socket.NativeAttachedAuthoredRootPreserved")
bool FOriginSocketNativeParent::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USCS_Node* Root = F.Node(TEXT("Root"));
    Root->bIsParentComponentNative = true;
    Root->ParentComponentOrVariableName = TEXT("NativeParent");
    Root->AttachToName = TEXT("InheritedSocket");
    F.Node(TEXT("ChildA"))->AttachToName = TEXT("GripSocket");
    const FTransform Before = F.Relative(TEXT("ChildA"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Socket insertion within unchanged authored root"),
        FOriginHierarchyAuthoring::InsertPivotAboveChild(F.Pivot(), TEXT("ChildA"), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Unchanged native-parent metadata"),
        Root->bIsParentComponentNative && Root->ParentComponentOrVariableName == TEXT("NativeParent") &&
        Root->AttachToName == TEXT("InheritedSocket"));
    TestTrue(TEXT("Local socket handoff preserved"),
        F.Node(Created)->AttachToName == TEXT("GripSocket") &&
        F.Node(TEXT("ChildA"))->AttachToName.IsNone() &&
        (F.Relative(TEXT("ChildA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

// The sibling grouping path is the useful skeletal-attachment workflow:
// SkeletalMesh authored in SCS -> several same-socket components plus an
// unrelated Origin Pivot component. The new Pivot inherits the mesh socket.
namespace
{
    bool BuildSocketSiblingFixture(FBatchFixture& F, FAutomationTestBase& Test, bool bTransactions = false)
    {
        if (!F.Build(Test, bTransactions)) return false;
        USimpleConstructionScript* Script = F.SCS();
        USCS_Node* Root = F.Node(TEXT("Root"));
        if (!IsValid(Root)) return false;
        USCS_Node* Mesh = Script->CreateNode(USkeletalMeshComponent::StaticClass(), TEXT("SocketMesh"));
        if (!Test.TestNotNull(TEXT("Authored Skeletal Mesh parent"), Mesh)) return false;
        Root->AddChildNode(Mesh, true);
        USCS_Node* AuthoringPivot = Script->CreateNode(UOriginPivotComponent::StaticClass(), TEXT("SocketAuthoringPivot"));
        USCS_Node* A = Script->CreateNode(USceneComponent::StaticClass(), TEXT("SocketSiblingA"));
        USCS_Node* B = Script->CreateNode(USceneComponent::StaticClass(), TEXT("SocketSiblingB"));
        if (!Test.TestNotNull(TEXT("Socket Pivot"), AuthoringPivot) ||
            !Test.TestNotNull(TEXT("Socket child A"), A) || !Test.TestNotNull(TEXT("Socket child B"), B)) return false;
        Mesh->AddChildNode(AuthoringPivot, true);
        Mesh->AddChildNode(A, true);
        Mesh->AddChildNode(B, true);
        A->AttachToName = TEXT("HandSocket");
        B->AttachToName = TEXT("HandSocket");
        auto Set = [](USCS_Node* Node, const FTransform& Desired)
        {
            if (USceneComponent* T = Cast<USceneComponent>(Node->ComponentTemplate.Get()))
                T->SetRelativeTransform_Direct(Desired);
        };
        Set(A, FTransform(FRotator(0, 20, 0), FVector(10, 20, 30), FVector::OneVector));
        Set(B, FTransform(FRotator(0, -15, 0), FVector(-25, 5, 12), FVector::OneVector));
        Set(AuthoringPivot, FTransform(FRotator::ZeroRotator, FVector(5, 0, 0), FVector::OneVector));
        FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
        return Test.TestNotNull(TEXT("Compiled skeletal parent fixture"), F.Node(TEXT("SocketMesh")));
    }
}

BATCH_TEST(FOriginSocketSiblingTwo, "Origin.Editor.Hierarchy.Blueprint.Socket.SiblingGroupSameMeshSocket")
bool FOriginSocketSiblingTwo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildSocketSiblingFixture(F, *this)) return false;
    const FTransform BeforeA = F.Relative(TEXT("SocketSiblingA"));
    const FTransform BeforeB = F.Relative(TEXT("SocketSiblingB"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group skeletal mesh socket siblings"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("SocketAuthoringPivot")),
            Names({TEXT("SocketSiblingA"), TEXT("SocketSiblingB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("New Origin Pivot attached beneath same Skeletal Mesh"),
        F.IsUnder(Created, TEXT("SocketMesh")) && F.Node(Created)->AttachToName == TEXT("HandSocket"));
    TestTrue(TEXT("Both children now normal children of new Pivot"),
        F.IsUnder(TEXT("SocketSiblingA"), Created) && F.IsUnder(TEXT("SocketSiblingB"), Created) &&
        F.Node(TEXT("SocketSiblingA"))->AttachToName.IsNone() &&
        F.Node(TEXT("SocketSiblingB"))->AttachToName.IsNone());
    TestTrue(TEXT("A preserves mesh socket frame"),
        (F.Relative(TEXT("SocketSiblingA")) * F.Relative(Created)).Equals(BeforeA, 1.e-3));
    TestTrue(TEXT("B preserves mesh socket frame"),
        (F.Relative(TEXT("SocketSiblingB")) * F.Relative(Created)).Equals(BeforeB, 1.e-3));
    TestTrue(TEXT("Reference Pivot untouched"), F.IsUnder(TEXT("SocketAuthoringPivot"), TEXT("SocketMesh")));
    return true;
}

BATCH_TEST(FOriginSocketSiblingOne, "Origin.Editor.Hierarchy.Blueprint.Socket.SiblingGroupOneSocketChild")
bool FOriginSocketSiblingOne::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildSocketSiblingFixture(F, *this)) return false;
    const FTransform Before = F.Relative(TEXT("SocketSiblingA"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group one socket-attached sibling"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("SocketAuthoringPivot")),
            Names({TEXT("SocketSiblingA")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("HandSocket moved to new Pivot"), F.IsUnder(Created, TEXT("SocketMesh")) &&
        F.Node(Created)->AttachToName == TEXT("HandSocket") && F.IsUnder(TEXT("SocketSiblingA"), Created) &&
        F.Node(TEXT("SocketSiblingA"))->AttachToName.IsNone());
    TestTrue(TEXT("Other socket sibling not touched"), F.IsUnder(TEXT("SocketSiblingB"), TEXT("SocketMesh")) &&
        F.Node(TEXT("SocketSiblingB"))->AttachToName == TEXT("HandSocket"));
    TestTrue(TEXT("Placement"), (F.Relative(TEXT("SocketSiblingA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketSiblingReject, "Origin.Editor.Hierarchy.Blueprint.Socket.SiblingRejectMixedMeshSockets")
bool FOriginSocketSiblingReject::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildSocketSiblingFixture(F, *this)) return false;
    F.Node(TEXT("SocketSiblingB"))->AttachToName = TEXT("HeadSocket");
    const FTransform Before = F.Relative(TEXT("SocketSiblingA"));
    const int32 Count = F.SCS()->GetAllNodes().Num();
    FName Created;
    FText Reason;
    TestFalse(TEXT("Different skeletal sockets rejected before mutation"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("SocketAuthoringPivot")),
            Names({TEXT("SocketSiblingA"), TEXT("SocketSiblingB")}), Created, Reason));
    TestTrue(TEXT("No new Pivot or partial transfer"), Created.IsNone() &&
        Count == F.SCS()->GetAllNodes().Num() &&
        F.IsUnder(TEXT("SocketSiblingA"), TEXT("SocketMesh")) &&
        F.Node(TEXT("SocketSiblingA"))->AttachToName == TEXT("HandSocket") &&
        F.Node(TEXT("SocketSiblingB"))->AttachToName == TEXT("HeadSocket") &&
        F.Relative(TEXT("SocketSiblingA")).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketSiblingList, "Origin.Editor.Hierarchy.Blueprint.Socket.SiblingDiscoverySeparatesInsertAndReparent")
bool FOriginSocketSiblingList::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildSocketSiblingFixture(F, *this)) return false;
    TArray<FName> Siblings, Children, Insertable;
    FOriginHierarchyAuthoring::ListSiblingsAndChildren(F.Pivot(TEXT("SocketAuthoringPivot")), Siblings, Children);
    FOriginHierarchyAuthoring::ListInsertableSiblings(F.Pivot(TEXT("SocketAuthoringPivot")), Insertable);
    TestFalse(TEXT("Ordinary reparent excludes socket siblings"), Siblings.Contains(TEXT("SocketSiblingA")));
    TestTrue(TEXT("Insertion discovery includes both socket siblings"),
        Insertable.Contains(TEXT("SocketSiblingA")) && Insertable.Contains(TEXT("SocketSiblingB")));
    TestTrue(TEXT("Discovery is nonmutating"), F.IsUnder(TEXT("SocketSiblingA"), TEXT("SocketMesh")) &&
        F.Node(TEXT("SocketSiblingA"))->AttachToName == TEXT("HandSocket"));
    return true;
}

BATCH_TEST(FOriginSocketSiblingCompile, "Origin.Editor.Hierarchy.Blueprint.Socket.SiblingGroupSurvivesCompile")
bool FOriginSocketSiblingCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildSocketSiblingFixture(F, *this)) return false;
    const FTransform Before = F.Relative(TEXT("SocketSiblingA"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group same-socket skeletal siblings"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("SocketAuthoringPivot")),
            Names({TEXT("SocketSiblingA"), TEXT("SocketSiblingB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Socket metadata survives recompile"), IsValid(F.Node(Created)) &&
        F.Node(Created)->AttachToName == TEXT("HandSocket") &&
        F.IsUnder(Created, TEXT("SocketMesh")) &&
        F.IsUnder(TEXT("SocketSiblingA"), Created));
    TestTrue(TEXT("Socket space placement survives compile"),
        (F.Relative(TEXT("SocketSiblingA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginSocketSiblingUndo, "Origin.Editor.Hierarchy.Blueprint.Socket.Transaction.SiblingGroupUndoRedo")
bool FOriginSocketSiblingUndo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildSocketSiblingFixture(F, *this, true)) return false;
    const FTransform Before = F.Relative(TEXT("SocketSiblingA"));
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Private Undo buffer"), Buffer)) return false;
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Transactional sibling socket group"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("SocketAuthoringPivot")),
            Names({TEXT("SocketSiblingA"), TEXT("SocketSiblingB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("One Undo transaction"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
    TestTrue(TEXT("Undo restores two original socket siblings"),
        F.IsUnder(TEXT("SocketSiblingA"), TEXT("SocketMesh")) &&
        F.IsUnder(TEXT("SocketSiblingB"), TEXT("SocketMesh")) &&
        F.Node(TEXT("SocketSiblingA"))->AttachToName == TEXT("HandSocket") &&
        F.Node(TEXT("SocketSiblingB"))->AttachToName == TEXT("HandSocket") &&
        F.Relative(TEXT("SocketSiblingA")).Equals(Before, 1.e-3));
    if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
    TestTrue(TEXT("Redo restores socket group"),
        F.IsUnder(Created, TEXT("SocketMesh")) && F.Node(Created)->AttachToName == TEXT("HandSocket") &&
        F.IsUnder(TEXT("SocketSiblingA"), Created) && F.IsUnder(TEXT("SocketSiblingB"), Created) &&
        (F.Relative(TEXT("SocketSiblingA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}
// UE native Scene Components (e.g. ACharacter::Mesh) are not SCS nodes.
// This fixture creates real authored SCS roots attached through native-parent
// metadata, which is what the Third Person template uses. It resolves the
// engine's native component template instead of making a fake authored parent.
namespace
{
    bool BuildExternalMeshFixture(FBatchFixture& F, FAutomationTestBase& Test,
        bool bTransactional = false)
    {
        if (!F.Build(Test, bTransactional, nullptr, NAME_None, ACharacter::StaticClass())) return false;
        USimpleConstructionScript* SCS = F.SCS();
        USCS_Node* Pivot = SCS->CreateNode(UOriginPivotComponent::StaticClass(), TEXT("NativeMeshPivot"));
        USCS_Node* A = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("NativeSocketA"));
        USCS_Node* B = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("NativeSocketB"));
        USCS_Node* C = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("NativeFreeC"));
        if (!Test.TestNotNull(TEXT("Native-parent pivot root"), Pivot) ||
            !Test.TestNotNull(TEXT("Native sibling A"), A) ||
            !Test.TestNotNull(TEXT("Native sibling B"), B) ||
            !Test.TestNotNull(TEXT("Native sibling C"), C)) return false;
        for (USCS_Node* Node : {Pivot, A, B, C})
        {
            Node->bIsParentComponentNative = true;
            // Owner class is used for inherited Blueprint parents, not native
            // components. The direct C++ parent remains native in this fixture.
            Node->ParentComponentOwnerClassName = NAME_None;
            Node->ParentComponentOrVariableName = TEXT("Mesh");
            SCS->AddNode(Node);
        }
        // The native component's object name is CharacterMesh0, while its
        // inherited property name is Mesh. Use whichever name UE's actual
        // native template resolution accepts, without relying on an SCS node.
        if (!Pivot->GetParentComponentTemplate(F.Blueprint.Get()))
            for (USCS_Node* Node : {Pivot, A, B, C})
                Node->ParentComponentOrVariableName = TEXT("CharacterMesh0");
        if (!Test.TestNotNull(TEXT("Resolved actual ACharacter native Mesh parent"),
            Pivot->GetParentComponentTemplate(F.Blueprint.Get()))) return false;
        if (!Test.TestTrue(TEXT("Same real external template resolved for siblings"),
            A->GetParentComponentTemplate(F.Blueprint.Get()) == Pivot->GetParentComponentTemplate(F.Blueprint.Get()) &&
            B->GetParentComponentTemplate(F.Blueprint.Get()) == Pivot->GetParentComponentTemplate(F.Blueprint.Get()))) return false;
        A->AttachToName = TEXT("hand_r");
        B->AttachToName = TEXT("hand_r");
        Cast<USceneComponent>(A->ComponentTemplate.Get())->SetRelativeTransform_Direct(
            FTransform(FRotator(0, 20, 0), FVector(60, 0, 10), FVector::OneVector));
        Cast<USceneComponent>(B->ComponentTemplate.Get())->SetRelativeTransform_Direct(
            FTransform(FRotator(0, -15, 0), FVector(20, 40, -5), FVector::OneVector));
        Cast<USceneComponent>(C->ComponentTemplate.Get())->SetRelativeTransform_Direct(
            FTransform(FRotator::ZeroRotator, FVector(5, 20, 0), FVector::OneVector));
        FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(),
            EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
        return Test.TestTrue(TEXT("Character Blueprint native parent survives compile"),
            IsValid(F.Node(TEXT("NativeMeshPivot"))) &&
            F.Node(TEXT("NativeMeshPivot"))->GetParentComponentTemplate(F.Blueprint.Get()) != nullptr);
    }
}

BATCH_TEST(FOriginNativeRootDiscovery, "Origin.Editor.Hierarchy.Blueprint.NativeParent.DiscoveryAndDetailsEligibility")
bool FOriginNativeRootDiscovery::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    UOriginPivotComponent* Pivot = F.Pivot(TEXT("NativeMeshPivot"));
    TestFalse(TEXT("General authored-parent operations stay disabled"),
        FOriginHierarchyAuthoring::CanEdit(Pivot));
    TestTrue(TEXT("Safe external-parent grouping is enabled"),
        FOriginHierarchyAuthoring::CanGroupExternalParentSiblings(Pivot));
    TArray<FName> Eligible, SocketAttached;
    FOriginHierarchyAuthoring::ListExternalParentSiblings(Pivot, Eligible, SocketAttached);
    TestTrue(TEXT("Find authored roots attached to native Character Mesh"),
        Eligible.Contains(TEXT("NativeSocketA")) && Eligible.Contains(TEXT("NativeSocketB")) &&
        Eligible.Contains(TEXT("NativeFreeC")) && !Eligible.Contains(TEXT("NativeMeshPivot")));
    TestTrue(TEXT("Correct socket-marked subset"), SocketAttached.Contains(TEXT("NativeSocketA")) &&
        SocketAttached.Contains(TEXT("NativeSocketB")) && !SocketAttached.Contains(TEXT("NativeFreeC")));
    return true;
}

BATCH_TEST(FOriginNativeRootGrouping, "Origin.Editor.Hierarchy.Blueprint.NativeParent.GroupSocketSiblingsPreservesFrame")
bool FOriginNativeRootGrouping::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    const FTransform ABefore = F.Relative(TEXT("NativeSocketA"));
    const FTransform BBefore = F.Relative(TEXT("NativeSocketB"));
    const USCS_Node* Source = F.Node(TEXT("NativeMeshPivot"));
    const FName ParentName = Source->ParentComponentOrVariableName;
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group sockets beneath the real native Character Mesh"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    USCS_Node* Group = F.Node(Created);
    if (!TestNotNull(TEXT("New Pivot SCS root"), Group)) return false;
    TestTrue(TEXT("New Pivot retains same untouched native parent"),
        F.SCS()->GetRootNodes().Contains(Group) &&
        Group->bIsParentComponentNative &&
        Group->ParentComponentOrVariableName == ParentName &&
        Group->GetParentComponentTemplate(F.Blueprint.Get()) == Source->GetParentComponentTemplate(F.Blueprint.Get()));
    TestTrue(TEXT("Single original socket belongs to the new Pivot"), Group->AttachToName == TEXT("hand_r"));
    TestTrue(TEXT("Authored children reparented, no stale native metadata"),
        F.IsUnder(TEXT("NativeSocketA"), Created) && F.IsUnder(TEXT("NativeSocketB"), Created) &&
        !F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.Node(TEXT("NativeSocketA"))->ParentComponentOrVariableName.IsNone() &&
        F.Node(TEXT("NativeSocketA"))->AttachToName.IsNone());
    TestTrue(TEXT("Former SCS roots remain registered for Blueprint preview selection"),
        F.SCS()->GetAllNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.SCS()->GetAllNodes().Contains(F.Node(TEXT("NativeSocketB"))) &&
        F.SCS()->FindSCSNode(TEXT("NativeSocketA")) == F.Node(TEXT("NativeSocketA")) &&
        F.SCS()->FindSCSNode(TEXT("NativeSocketB")) == F.Node(TEXT("NativeSocketB")));
    TestTrue(TEXT("A preserved in native socket space"),
        (F.Relative(TEXT("NativeSocketA")) * F.Relative(Created)).Equals(ABefore, 1.e-3));
    TestTrue(TEXT("B preserved in native socket space"),
        (F.Relative(TEXT("NativeSocketB")) * F.Relative(Created)).Equals(BBefore, 1.e-3));
    TestTrue(TEXT("Reference pivot and unrelated sibling remain unchanged"),
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeMeshPivot"))) &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeFreeC"))));
    return true;
}

BATCH_TEST(FOriginNativeRootRegistry, "Origin.Editor.Hierarchy.Blueprint.NativeParent.RegistryPreservedAcrossRootReparent")
bool FOriginNativeRootRegistry::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    const int32 CountBefore = F.SCS()->GetAllNodes().Num();
    FName Grouped;
    FText Reason;
    if (!TestTrue(TEXT("Group native-parent children"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Grouped, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Exactly one new SCS node, no node lost during root reparent"),
        F.SCS()->GetAllNodes().Num(), CountBefore + 1);
    TestTrue(TEXT("Former roots indexed exactly once"),
        F.SCS()->GetAllNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.SCS()->GetAllNodes().Contains(F.Node(TEXT("NativeSocketB"))) &&
        F.SCS()->FindSCSNode(TEXT("NativeSocketA")) != nullptr &&
        F.SCS()->FindSCSNode(TEXT("NativeSocketB")) != nullptr &&
        F.IsUnder(TEXT("NativeSocketA"), Grouped) && F.IsUnder(TEXT("NativeSocketB"), Grouped));
    return true;
}

BATCH_TEST(FOriginNativeRootNestedRegistry, "Origin.Editor.Hierarchy.Blueprint.NativeParent.RegistryPreservesNestedBranch")
bool FOriginNativeRootNestedRegistry::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    USCS_Node* Child = F.Node(TEXT("NativeSocketA"));
    USCS_Node* Grandchild = F.SCS()->CreateNode(USceneComponent::StaticClass(),
        TEXT("NativeSocketGrandchild"));
    if (!TestNotNull(TEXT("Authored nested node"), Grandchild)) return false;
    Child->AddChildNode(Grandchild, true);
    const int32 Before = F.SCS()->GetAllNodes().Num();
    FName Grouped;
    FText Reason;
    if (!TestTrue(TEXT("Group native socket branch with descendant"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Grouped, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Every existing node stays in AllNodes"), F.SCS()->GetAllNodes().Num(), Before + 1);
    TestTrue(TEXT("Nested node remains discoverable under grouped child"),
        F.SCS()->FindSCSNode(TEXT("NativeSocketGrandchild")) == Grandchild &&
        F.SCS()->GetAllNodes().Contains(Grandchild) &&
        F.IsUnder(TEXT("NativeSocketGrandchild"), TEXT("NativeSocketA")) &&
        F.IsUnder(TEXT("NativeSocketA"), Grouped));
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(),
        EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Nested grouped branch remains discoverable after Blueprint compile"),
        F.SCS()->FindSCSNode(TEXT("NativeSocketGrandchild")) != nullptr &&
        F.IsUnder(TEXT("NativeSocketGrandchild"), TEXT("NativeSocketA")));
    return true;
}

BATCH_TEST(FOriginNativeRootReuse, "Origin.Editor.Hierarchy.Blueprint.NativeParent.AdoptIntoSelectedPivotWithoutDuplicate")
bool FOriginNativeRootReuse::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    const int32 CountBefore = F.SCS()->GetAllNodes().Num();
    const FTransform BeforeA = F.Relative(TEXT("NativeSocketA"));
    const FTransform BeforeB = F.Relative(TEXT("NativeSocketB"));
    UOriginPivotComponent* Pivot = F.Pivot(TEXT("NativeMeshPivot"));
    USCS_Node* PivotNode = F.Node(TEXT("NativeMeshPivot"));
    if (!TestTrue(TEXT("Empty reference Pivot can adopt"),
        FOriginHierarchyAuthoring::CanAdoptExternalParentSiblings(Pivot))) return false;
    FText Reason;
    if (!TestTrue(TEXT("Group into the selected Pivot, rather than creating another"),
        FOriginHierarchyAuthoring::AdoptExternalParentSiblings(Pivot,
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("No additional Origin Pivot SCS node created"),
        F.SCS()->GetAllNodes().Num(), CountBefore);
    TestTrue(TEXT("Selected Pivot keeps its identity and native parent"),
        F.Node(TEXT("NativeMeshPivot")) == PivotNode &&
        F.SCS()->GetRootNodes().Contains(PivotNode) &&
        PivotNode->bIsParentComponentNative &&
        PivotNode->GetParentComponentTemplate(F.Blueprint.Get()) != nullptr &&
        PivotNode->AttachToName == TEXT("hand_r"));
    TestTrue(TEXT("Both authored children become normal, discoverable descendants"),
        F.IsUnder(TEXT("NativeSocketA"), TEXT("NativeMeshPivot")) &&
        F.IsUnder(TEXT("NativeSocketB"), TEXT("NativeMeshPivot")) &&
        F.Node(TEXT("NativeSocketA"))->AttachToName.IsNone() &&
        F.SCS()->GetAllNodes().Contains(F.Node(TEXT("NativeSocketB"))));
    TestTrue(TEXT("Both socket-space transforms are compensated"),
        (F.Relative(TEXT("NativeSocketA")) * F.Relative(TEXT("NativeMeshPivot"))).Equals(BeforeA, 1.e-3) &&
        (F.Relative(TEXT("NativeSocketB")) * F.Relative(TEXT("NativeMeshPivot"))).Equals(BeforeB, 1.e-3));
    return true;
}

BATCH_TEST(FOriginNativeRootReuseCompile, "Origin.Editor.Hierarchy.Blueprint.NativeParent.AdoptSurvivesCompile")
bool FOriginNativeRootReuseCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    const FTransform Before = F.Relative(TEXT("NativeSocketA"));
    FText Reason;
    if (!TestTrue(TEXT("Group into existing native-root Pivot"),
        FOriginHierarchyAuthoring::AdoptExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(),
        EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Selected Pivot and children survive full Blueprint compile"),
        F.Node(TEXT("NativeMeshPivot")) != nullptr &&
        F.Node(TEXT("NativeMeshPivot"))->AttachToName == TEXT("hand_r") &&
        F.IsUnder(TEXT("NativeSocketA"), TEXT("NativeMeshPivot")) &&
        F.IsUnder(TEXT("NativeSocketB"), TEXT("NativeMeshPivot")) &&
        (F.Relative(TEXT("NativeSocketA")) * F.Relative(TEXT("NativeMeshPivot"))).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginNativeRootReuseUndo, "Origin.Editor.Hierarchy.Blueprint.NativeParent.Transaction.AdoptUndoRedo")
bool FOriginNativeRootReuseUndo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this, true)) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Undo buffer"), Buffer)) return false;
    const FTransform Before = F.Relative(TEXT("NativeSocketA"));
    FText Reason;
    if (!TestTrue(TEXT("Adopt into selected native-parent Pivot"),
        FOriginHierarchyAuthoring::AdoptExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("Exactly one Undo transaction"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
    TestTrue(TEXT("Undo restores prior root and socket hierarchy"),
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.Node(TEXT("NativeSocketA"))->AttachToName == TEXT("hand_r") &&
        F.Node(TEXT("NativeMeshPivot"))->AttachToName.IsNone() &&
        F.Relative(TEXT("NativeSocketA")).Equals(Before, 1.e-3));
    if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
    TestTrue(TEXT("Redo groups into the same Pivot without node loss"),
        F.IsUnder(TEXT("NativeSocketA"), TEXT("NativeMeshPivot")) &&
        F.Node(TEXT("NativeMeshPivot"))->AttachToName == TEXT("hand_r") &&
        F.SCS()->GetAllNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        (F.Relative(TEXT("NativeSocketA")) * F.Relative(TEXT("NativeMeshPivot"))).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginNativeRootReuseGuard, "Origin.Editor.Hierarchy.Blueprint.NativeParent.AdoptRejectsOccupiedPivot")
bool FOriginNativeRootReuseGuard::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    USCS_Node* Occupant = F.SCS()->CreateNode(USceneComponent::StaticClass(), TEXT("AlreadyUnderPivot"));
    F.Node(TEXT("NativeMeshPivot"))->AddChildNode(Occupant, true);
    const int32 Before = F.SCS()->GetAllNodes().Num();
    TestFalse(TEXT("Occupied Pivot not eligible for auto-adoption"),
        FOriginHierarchyAuthoring::CanAdoptExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot"))));
    FText Reason;
    TestFalse(TEXT("Auto-adoption refuses to change an existing assembly's socket frame"),
        FOriginHierarchyAuthoring::AdoptExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA")}), Reason));
    TestTrue(TEXT("No partial mutation"),
        Before == F.SCS()->GetAllNodes().Num() &&
        F.IsUnder(TEXT("AlreadyUnderPivot"), TEXT("NativeMeshPivot")) &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.Node(TEXT("NativeMeshPivot"))->AttachToName.IsNone());
    return true;
}

BATCH_TEST(FOriginNativeRootPoseGuard, "Origin.Editor.Hierarchy.Blueprint.NativeParent.AdoptRejectsCapturedPivotPose")
bool FOriginNativeRootPoseGuard::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    UOriginPivotComponent* Pivot = F.Pivot(TEXT("NativeMeshPivot"));
    if (!TestNotNull(TEXT("Reference Pivot"), Pivot)) return false;
    // Changing the Pivot socket would make captured slot frames stale.
    Pivot->OriginPivotConfiguration.Slots[0].bHasSavedRelativeTransform = true;
    Pivot->OriginPivotConfiguration.Slots[0].CapturedSocketName = NAME_None;
    TestFalse(TEXT("Captured poses disable auto-adoption"),
        FOriginHierarchyAuthoring::CanAdoptExternalParentSiblings(Pivot));
    const int32 CountBefore = F.SCS()->GetAllNodes().Num();
    FText Reason;
    TestFalse(TEXT("Adopt refuses to invalidate captured pose socket frame"),
        FOriginHierarchyAuthoring::AdoptExternalParentSiblings(Pivot,
            Names({TEXT("NativeSocketA")}), Reason));
    TestTrue(TEXT("No structural changes to saved-pose Pivot"),
        CountBefore == F.SCS()->GetAllNodes().Num() &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.Node(TEXT("NativeMeshPivot"))->AttachToName.IsNone());
    return true;
}

BATCH_TEST(FOriginNativeRootMixedSockets, "Origin.Editor.Hierarchy.Blueprint.NativeParent.RejectDifferentSocketsAtomically")
bool FOriginNativeRootMixedSockets::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    F.Node(TEXT("NativeSocketB"))->AttachToName = TEXT("head");
    const int32 Before = F.SCS()->GetAllNodes().Num();
    FName Created;
    FText Reason;
    TestFalse(TEXT("Distinct native Mesh sockets reject without mutation"),
        FOriginHierarchyAuthoring::InsertSharedPivotAboveSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Created, Reason));
    TestTrue(TEXT("No SCS mutation"), Created.IsNone() && Before == F.SCS()->GetAllNodes().Num() &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketB"))) &&
        F.Node(TEXT("NativeSocketA"))->AttachToName == TEXT("hand_r") &&
        F.Node(TEXT("NativeSocketB"))->AttachToName == TEXT("head"));
    return true;
}

BATCH_TEST(FOriginNativeRootMixedAttachment, "Origin.Editor.Hierarchy.Blueprint.NativeParent.RejectSocketAndNonSocketMix")
bool FOriginNativeRootMixedAttachment::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    const int32 Before = F.SCS()->GetAllNodes().Num();
    FName Created;
    FText Reason;
    TestFalse(TEXT("Cannot merge different parent frames"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeFreeC")}), Created, Reason));
    TestTrue(TEXT("No partial edit"), Created.IsNone() && Before == F.SCS()->GetAllNodes().Num());
    return true;
}

BATCH_TEST(FOriginNativeRootDuplicate, "Origin.Editor.Hierarchy.Blueprint.NativeParent.RejectInvalidSelectionAtomically")
bool FOriginNativeRootDuplicate::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    FName Created;
    FText Reason;
    TestFalse(TEXT("Reject source Pivot in selection"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeMeshPivot")}), Created, Reason));
    TestFalse(TEXT("Reject duplicated child"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketA")}), Created, Reason));
    TestFalse(TEXT("Reject nonexistent child"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("MissingChild")}), Created, Reason));
    TestTrue(TEXT("No partial SCS topology changes"),
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketB"))));
    return true;
}

BATCH_TEST(FOriginNativeRootCompile, "Origin.Editor.Hierarchy.Blueprint.NativeParent.GroupSurvivesCompile")
bool FOriginNativeRootCompile::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    const FTransform Before = F.Relative(TEXT("NativeSocketA"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group under native parent"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(),
        EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    USCS_Node* Group = F.Node(Created);
    TestTrue(TEXT("Native parent and socket survive Blueprint compile"), IsValid(Group) &&
        F.SCS()->GetRootNodes().Contains(Group) &&
        Group->bIsParentComponentNative && Group->AttachToName == TEXT("hand_r") &&
        Group->GetParentComponentTemplate(F.Blueprint.Get()) != nullptr &&
        F.IsUnder(TEXT("NativeSocketA"), Created) &&
        (F.Relative(TEXT("NativeSocketA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginNativeRootUndo, "Origin.Editor.Hierarchy.Blueprint.NativeParent.Transaction.UndoRedo")
bool FOriginNativeRootUndo::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this, true)) return false;
    FPrivateUndo Undo;
    UTransBuffer* Buffer = Undo.Get();
    if (!TestNotNull(TEXT("Undo buffer"), Buffer)) return false;
    const FTransform Before = F.Relative(TEXT("NativeSocketA"));
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Native-parent group in one transaction"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeSocketA"), TEXT("NativeSocketB")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestEqual(TEXT("One Undo step"), Buffer->GetQueueLength(), 1);
    if (!TestTrue(TEXT("Undo"), Buffer->Undo(true))) return false;
    TestTrue(TEXT("Undo restores two native-attached socket roots"),
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketA"))) &&
        F.SCS()->GetRootNodes().Contains(F.Node(TEXT("NativeSocketB"))) &&
        F.Node(TEXT("NativeSocketA"))->AttachToName == TEXT("hand_r") &&
        F.Relative(TEXT("NativeSocketA")).Equals(Before, 1.e-3));
    if (!TestTrue(TEXT("Redo"), Buffer->Redo())) return false;
    TestTrue(TEXT("Redo restores native-attached grouped socket"),
        IsValid(F.Node(Created)) && F.SCS()->GetRootNodes().Contains(F.Node(Created)) &&
        F.Node(Created)->AttachToName == TEXT("hand_r") &&
        F.IsUnder(TEXT("NativeSocketA"), Created) &&
        (F.Relative(TEXT("NativeSocketA")) * F.Relative(Created)).Equals(Before, 1.e-3));
    return true;
}

BATCH_TEST(FOriginNativeRootNoSocket, "Origin.Editor.Hierarchy.Blueprint.NativeParent.GroupUnattachedSiblings")
bool FOriginNativeRootNoSocket::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!BuildExternalMeshFixture(F, *this)) return false;
    FName Created;
    FText Reason;
    if (!TestTrue(TEXT("Group socket-free native-parent siblings"),
        FOriginHierarchyAuthoring::GroupExternalParentSiblings(F.Pivot(TEXT("NativeMeshPivot")),
            Names({TEXT("NativeFreeC")}), Created, Reason)))
    { AddError(Reason.ToString()); return false; }
    TestTrue(TEXT("Group retains native parent and no socket"),
        F.SCS()->GetRootNodes().Contains(F.Node(Created)) &&
        F.Node(Created)->AttachToName.IsNone() && F.IsUnder(TEXT("NativeFreeC"), Created));
    return true;
}

// Opt-in disk persistence integration. Deliberately outside Origin.Editor.* so
// Origin.Tests.Regression stays fast and does not perform package reloads.
BATCH_TEST(FOriginSavedBlueprintPersistence, "Origin.Integration.Persistence.BlueprintTransferSaveReload")
bool FOriginSavedBlueprintPersistence::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld)
    {
        AddError(TEXT("Run the persistence integration test in an idle editor outside PIE."));
        return false;
    }
    // A unique virtual mount backed ONLY by Saved/, never Content/ or an existing asset.
    // No user package is modified or reloaded by this test.
    const FString ID = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    // Use a unique nested /Game/ path for standard editor asset-path handling.
    // The nested mount redirects ONLY this scratch subtree to Saved/, so no
    // user Content asset is created, edited, or removed.
    const FString MountRoot = FString::Printf(TEXT("/Game/__OriginPersistenceQA_%s/"), *ID);
    FString DiskRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OriginPersistenceQA"), ID);
    FPaths::NormalizeDirectoryName(DiskRoot);
    DiskRoot += TEXT("/");
    if (!IFileManager::Get().MakeDirectory(*DiskRoot, true))
    {
        AddError(TEXT("Could not create a private Origin persistence test directory under Saved/."));
        return false;
    }
    // A package reload can enqueue editor DataValidation for the next tick.
    // Do not unmount/delete the fixture synchronously on RunTest's return.
    // Keep it mounted for subsequent editor ticks, then remove it once the
    // deferred editor callbacks have had a chance to process the valid path.
    const auto RegisteredMount = FPackageName::RegisterMountPoint(MountRoot, DiskRoot);
    if (!RegisteredMount.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
        AddError(TEXT("Could not register the private Origin persistence test mount."));
        return false;
    }
    struct FTestMountCleanup
    {
        FString Root, Disk;
        TStrongObjectPtr<UBlueprint> ReloadedBlueprint;
        FTestMountCleanup(const FString& InRoot, const FString& InDisk)
            : Root(InRoot), Disk(InDisk) {}
        ~FTestMountCleanup()
        {
            ReloadedBlueprint.Reset();
            FPackageName::UnRegisterMountPoint(Root, Disk);
            // Delete only this run's GUID-named directory beneath Saved/.
            IFileManager::Get().DeleteDirectory(*Disk, false, true);
        }
    };
    TSharedRef<FTestMountCleanup> Cleanup = MakeShared<FTestMountCleanup>(MountRoot, DiskRoot);

    const FString AssetName = FString::Printf(TEXT("OriginPersistence_%s"), *ID);
    const FString LongPackageName = MountRoot + AssetName;
    UPackage* Package = CreatePackage(*LongPackageName);
    if (!TestNotNull(TEXT("Dedicated mounted scratch package"), Package)) return false;
    const FString Filename = FPaths::Combine(DiskRoot, AssetName + FPackageName::GetAssetPackageExtension());
    // A nested /Game/ mount must resolve exactly to our Saved/ fixture path,
    // never to a similarly named folder in the project's Content directory.
    FString ResolvedFilename;
    if (!TestTrue(TEXT("Temporary package path resolves through the private Saved/ mount"),
        FPackageName::TryConvertLongPackageNameToFilename(LongPackageName,
            ResolvedFilename, FPackageName::GetAssetPackageExtension()))) return false;
    FString ExpectedPath = FPaths::ConvertRelativePathToFull(Filename);
    FString ActualPath = FPaths::ConvertRelativePathToFull(ResolvedFilename);
    FPaths::NormalizeFilename(ExpectedPath);
    FPaths::NormalizeFilename(ActualPath);
    if (!TestTrue(TEXT("Temporary mount does not resolve into user Content"),
        ExpectedPath.Equals(ActualPath, ESearchCase::IgnoreCase))) return false;
    FBatchFixture Fixture;
    if (!Fixture.Build(*this, false, Package, FName(*AssetName))) return false;
    if (!TestTrue(TEXT("Blueprint fixture is inside isolated scratch package"),
        Fixture.Blueprint->GetOutermost() == Package)) return false;
    const FTransform OriginalA = Fixture.InRoot(TEXT("ChildA"));
    const FTransform OriginalGrandchild = Fixture.InRoot(TEXT("Grandchild"));
    const FTransform OriginalB = Fixture.InRoot(TEXT("ChildB"));
    const FTransform OriginalC = Fixture.InRoot(TEXT("ChildC"));
    const FTransform OriginalBInSocket = Fixture.Relative(TEXT("ChildB"));
    const FTransform OriginalCInSocket = Fixture.Relative(TEXT("ChildC"));
    Fixture.Node(TEXT("ChildB"))->AttachToName = TEXT("GripSocket");
    Fixture.Node(TEXT("ChildC"))->AttachToName = TEXT("GripSocket");
    FName CreatedSocketPivot;
    // Physical fixtures use a transaction-bearing package, but must never
    // add an Undo entry to the user's normal editor history.
    {
        FPrivateUndo LocalUndo;
        UTransBuffer* Buffer = LocalUndo.Get();
        if (!TestNotNull(TEXT("Isolated integration-test transaction buffer"), Buffer)) return false;
        FText Reason;
        if (!TestTrue(TEXT("Transfer a branch before saving"),
            FOriginHierarchyAuthoring::TransferChildrenToPivot(Fixture.Pivot(), TEXT("Inner"),
                Names({TEXT("ChildA")}), Reason)))
        { AddError(Reason.ToString()); return false; }
        if (!TestTrue(TEXT("Insert a shared socket-attached Pivot before saving"),
            FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(Fixture.Pivot(),
                Names({TEXT("ChildB"), TEXT("ChildC")}), CreatedSocketPivot, Reason)))
        { AddError(Reason.ToString()); return false; }
        Buffer->Reset(FText::FromString(TEXT("Origin scratch persistence test completed its edits")));
    }
    FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint.Get(),
        EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    Fixture.Blueprint->SetFlags(RF_Public | RF_Standalone);
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    if (!TestTrue(TEXT("UE saved the edited Blueprint asset to its own private .uasset"),
        UPackage::SavePackage(Package, Fixture.Blueprint.Get(), *Filename, SaveArgs))) return false;
    if (!TestTrue(TEXT("Scratch .uasset is present on disk"), IFileManager::Get().FileExists(*Filename))) return false;

    // Release our explicit strong ref before asking UE to reload the scratch package.
    Fixture.Blueprint.Reset();
    UPackage* Reloaded = ReloadPackage(Package, LOAD_None);
    if (!TestNotNull(TEXT("Scratch package unloaded and reloaded from disk"), Reloaded)) return false;
    UBlueprint* ReloadedBP = FindObject<UBlueprint>(Reloaded, *AssetName);
    if (!TestNotNull(TEXT("Saved Blueprint reloaded"), ReloadedBP)) return false;
    Cleanup->ReloadedBlueprint.Reset(ReloadedBP);
    USimpleConstructionScript* Script = ReloadedBP->SimpleConstructionScript;
    if (!TestNotNull(TEXT("Reloaded authored SCS"), Script)) return false;
    auto Node = [Script](FName Name) { return Script->FindSCSNode(Name); };
    USCS_Node* Child = Node(TEXT("ChildA"));
    USCS_Node* Grandchild = Node(TEXT("Grandchild"));
    USCS_Node* ReloadedSocketPivot = Node(CreatedSocketPivot);
    if (!TestNotNull(TEXT("Reloaded moved child"), Child) ||
        !TestNotNull(TEXT("Reloaded grandchild"), Grandchild) ||
        !TestNotNull(TEXT("Reloaded shared socket Pivot"), ReloadedSocketPivot)) return false;
    TestTrue(TEXT("Transferred topology survived physical save/reload"),
        Script->FindParentNode(Child) == Node(TEXT("Inner")) &&
        Script->FindParentNode(Grandchild) == Child);
    auto InRoot = [&Node, Script](FName Name)
    {
        FTransform Total = FTransform::Identity;
        USCS_Node* Current = Node(Name);
        for (int32 Depth = 0; Depth < 128 && IsValid(Current) && Current != Node(TEXT("Root")); ++Depth)
        {
            const USceneComponent* Template = Cast<USceneComponent>(Current->ComponentTemplate.Get());
            if (!IsValid(Template)) return FTransform::Identity;
            Total = Total * Template->GetRelativeTransform();
            Current = Script->FindParentNode(Current);
        }
        return Total;
    };
    TestTrue(TEXT("Moved child placement persisted"), InRoot(TEXT("ChildA")).Equals(OriginalA, 1.e-3));
    TestTrue(TEXT("Grandchild placement persisted"), InRoot(TEXT("Grandchild")).Equals(OriginalGrandchild, 1.e-3));
    TestTrue(TEXT("Socket child B placement persisted"), InRoot(TEXT("ChildB")).Equals(OriginalB, 1.e-3));
    TestTrue(TEXT("Socket child C placement persisted"), InRoot(TEXT("ChildC")).Equals(OriginalC, 1.e-3));
    TestTrue(TEXT("Shared socket metadata survived actual disk reload"),
        ReloadedSocketPivot->AttachToName == TEXT("GripSocket") &&
        Script->FindParentNode(Node(TEXT("ChildB"))) == ReloadedSocketPivot &&
        Script->FindParentNode(Node(TEXT("ChildC"))) == ReloadedSocketPivot &&
        Node(TEXT("ChildB"))->AttachToName.IsNone() &&
        Node(TEXT("ChildC"))->AttachToName.IsNone());
    const USceneComponent* SocketPivotTemplate = Cast<USceneComponent>(ReloadedSocketPivot->ComponentTemplate.Get());
    const USceneComponent* SocketChildB = Cast<USceneComponent>(Node(TEXT("ChildB"))->ComponentTemplate.Get());
    const USceneComponent* SocketChildC = Cast<USceneComponent>(Node(TEXT("ChildC"))->ComponentTemplate.Get());
    TestTrue(TEXT("Socket-space transforms survived physical save/reload"),
        IsValid(SocketPivotTemplate) && IsValid(SocketChildB) && IsValid(SocketChildC) &&
        (SocketChildB->GetRelativeTransform() * SocketPivotTemplate->GetRelativeTransform()).Equals(OriginalBInSocket, 1.e-3) &&
        (SocketChildC->GetRelativeTransform() * SocketPivotTemplate->GetRelativeTransform()).Equals(OriginalCInSocket, 1.e-3));
    TestTrue(TEXT("Original Pivot and nested Pivot survived"),
        IsValid(Node(TEXT("Outer"))) && IsValid(Node(TEXT("Inner"))) &&
        Node(TEXT("Inner"))->ComponentTemplate->IsA<UOriginPivotComponent>());

    // Engine validators can be queued by ReloadPackage and execute on a later
    // frame. Retain the mount, saved bytes and Blueprint until after those
    // callbacks; the automation framework keeps this latent command alive.
    // No global DataValidation settings or other plugins are altered.
    const double ReleaseAfter = FPlatformTime::Seconds() + 1.0;
    AddCommand(new FFunctionLatentCommand([Cleanup, ReleaseAfter, FrameCount = 0]() mutable
    {
        ++FrameCount;
        return FrameCount >= 4 && FPlatformTime::Seconds() >= ReleaseAfter;
    }));
    return true;
}

#undef BATCH_TEST
#endif
