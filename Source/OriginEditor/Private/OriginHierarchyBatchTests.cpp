// Copyright Mippithedork 2026, Inc. All Rights Reserved.
#include "OriginHierarchyAuthoring.h"
#include <initializer_list>

#if WITH_DEV_AUTOMATION_TESTS
#include "OriginPivotComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
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
        bool Build(FAutomationTestBase& Test, bool bTransactional = false)
        {
            UPackage* Package = bTransactional
                ? CreatePackage(*FString::Printf(TEXT("/Temp/OriginBatchUndo_%s"),
                    *FGuid::NewGuid().ToString(EGuidFormats::Digits))) : GetTransientPackage();
            const FName BPName(*FString::Printf(TEXT("OriginBatchQA_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
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


BATCH_TEST(FOriginBatchRejectInheritedRoot, "Origin.Editor.Hierarchy.Blueprint.Batch.InheritedRootDetachRejectsAtomically")
bool FOriginBatchRejectInheritedRoot::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USCS_Node* Root = F.Node(TEXT("Root"));
    if (!TestNotNull(TEXT("Authored scene root"), Root)) return false;
    Root->bIsParentComponentNative = true; // Emulate an SCS root bound to a native parent.
    Root->ParentComponentOrVariableName = TEXT("NativeSceneRoot");
    const FTransform Before = F.InRoot(TEXT("InnerChild"));
    FText Reason;
    TestFalse(TEXT("Reject detach through native parent reference"),
        FOriginHierarchyAuthoring::DetachChildrenToRoot(F.Pivot(TEXT("Inner")),
            Names({TEXT("InnerChild")}), Reason));
    TestTrue(TEXT("Parent remains nested"), F.IsUnder(TEXT("InnerChild"), TEXT("Inner")));
    TestTrue(TEXT("Transform remains unchanged"), F.InRoot(TEXT("InnerChild")).Equals(Before, 1.e-3));
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

BATCH_TEST(FOriginTransferInherited, "Origin.Editor.Hierarchy.Blueprint.Transfer.InheritedRootRejectsAtomically")
bool FOriginTransferInherited::RunTest(const FString& Parameters)
{
    FBatchFixture F;
    if (!F.Build(*this)) return false;
    USCS_Node* Root = F.Node(TEXT("Root"));
    Root->bIsParentComponentNative = true;
    Root->ParentComponentOrVariableName = TEXT("NativeSceneRoot");
    const FTransform A = F.Relative(TEXT("ChildA"));
    FText Reason;
    TestFalse(TEXT("Native attached authored root unsupported"),
        FOriginHierarchyAuthoring::TransferChildrenToPivot(F.Pivot(), TEXT("Inner"),
            Names({TEXT("ChildA")}), Reason));
    TestTrue(TEXT("No mutation"), F.IsUnder(TEXT("ChildA"), TEXT("Outer")) &&
        F.Relative(TEXT("ChildA")).Equals(A, 1.e-3));
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

#undef BATCH_TEST
#endif
