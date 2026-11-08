// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Alpha 5.4: source math and host parity regressions. No user levels/assets.
#include "OriginPivotSnapMath.h"
#include "OriginPivotComponent.h"
#include "OriginAnchor.h"
#include "OriginPivotAuthoring.h"
#include "OriginAnchorAuthoring.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include <limits>

namespace
{
    bool Near(const FVector& A, const FVector& B) { return A.Equals(B, 0.01); }
    bool PoseNear(const FTransform& A, const FTransform& B)
    {
        return Near(A.GetLocation(), B.GetLocation()) &&
            A.GetRotation().Equals(B.GetRotation(), 1.e-4) &&
            A.GetScale3D().Equals(B.GetScale3D(), 1.e-4);
    }

    struct FTestInput
    {
        FOriginPivotCalculationInput Input;
        FOriginPivotDefinition Rule;
        FTestInput()
        {
            Input.AssemblyBounds = FBox(FVector(20, 40, -30), FVector(180, 260, 90));
            Input.DirectChildOrigins = {FVector(40, 100, 20), FVector(160, 200, 60)};
            Input.ReferencePosition = FVector(700, 800, 900);
            Input.bHasReferencePosition = true;
            Input.PivotRotation = FQuat(FVector::UpVector, UE_PI / 2.0);
        }
        bool Evaluate(FVector& Out, FString& Error) const
        { return FOriginPivotSnapMath::CalculatePosition(Rule, Input, Out, Error); }
    };

    struct FEditorCalculationFixture
    {
        FTestWorldWrapper TestWorld;
        UWorld* World = nullptr;
        AOriginAnchor* Anchor = nullptr;
        AActor* ChildA = nullptr;
        AActor* ChildB = nullptr;
        AActor* Owner = nullptr;
        UOriginPivotComponent* Pivot = nullptr;
        USceneComponent* SourceA = nullptr;
        USceneComponent* SourceB = nullptr;

        AActor* Spawn(const FVector& Where)
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* Actor = World->SpawnActor<AActor>(Where, FRotator::ZeroRotator, Params);
            if (!Actor) return nullptr;
            USceneComponent* Root = NewObject<USceneComponent>(Actor, NAME_None, RF_Transactional);
            if (!Root) return nullptr;
            Root->SetMobility(EComponentMobility::Movable);
            Actor->AddInstanceComponent(Root);
            Actor->SetRootComponent(Root);
            Root->RegisterComponent();
            Actor->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
            return Actor;
        }
        bool Setup(FAutomationTestBase& Test)
        {
            if (!Test.TestTrue(TEXT("Isolated editor world"), TestWorld.CreateTestWorld(EWorldType::Editor)))
                return false;
            World = TestWorld.GetTestWorld();
            if (!Test.TestNotNull(TEXT("Fixture world"), World)) return false;
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Anchor = World->SpawnActor<AOriginAnchor>(FVector(10, 20, 30), FRotator::ZeroRotator, Params);
            if (!Test.TestNotNull(TEXT("Fixture Anchor"), Anchor)) return false;
            Anchor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
            Owner = Spawn(FVector(0, 0, 0));
            ChildA = Spawn(FVector(100, 200, 50));
            ChildB = Spawn(FVector(300, 400, 90));
            if (!Owner || !ChildA || !ChildB) return false;
            Pivot = NewObject<UOriginPivotComponent>(Owner, TEXT("AuthoringPivot"), RF_Transactional);
            if (!Test.TestNotNull(TEXT("Fixture Pivot"), Pivot)) return false;
            Owner->AddInstanceComponent(Pivot);
            Pivot->SetupAttachment(Owner->GetRootComponent());
            Pivot->RegisterComponent();
            Pivot->SetWorldLocation(FVector(10, 20, 30));
            SourceA = NewObject<USceneComponent>(Owner, TEXT("SourceA"), RF_Transactional);
            SourceB = NewObject<USceneComponent>(Owner, TEXT("SourceB"), RF_Transactional);
            if (!SourceA || !SourceB) return false;
            for (USceneComponent* C : {SourceA, SourceB})
            {
                C->SetMobility(EComponentMobility::Movable);
                Owner->AddInstanceComponent(C);
                C->SetupAttachment(Pivot);
                C->RegisterComponent();
            }
            SourceA->SetWorldLocation(FVector(100, 200, 50));
            SourceB->SetWorldLocation(FVector(300, 400, 90));
            return Test.TestTrue(TEXT("Actor child A attached"), ChildA->AttachToActor(Anchor, FAttachmentTransformRules::KeepWorldTransform)) &&
                Test.TestTrue(TEXT("Actor child B attached"), ChildB->AttachToActor(Anchor, FAttachmentTransformRules::KeepWorldTransform));
        }
    };

    struct FAuthoredSourceFixture
    {
        TStrongObjectPtr<UBlueprint> Blueprint;
        bool Setup(FAutomationTestBase& Test)
        {
            UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/OriginCalc_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            const FName BPName(*FString::Printf(TEXT("OriginCalcQA_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
                Package, BPName, BPTYPE_Normal, TEXT("OriginCalcTests"));
            Blueprint.Reset(BP);
            if (!Test.TestNotNull(TEXT("Engine-created authored Blueprint"), BP)) return false;
            USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
            if (!Test.TestNotNull(TEXT("Source test SCS"), SCS)) return false;
            USCS_Node* Root = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Root"));
            USCS_Node* Pivot = SCS->CreateNode(UOriginPivotComponent::StaticClass(), TEXT("Pivot"));
            USCS_Node* Child = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Child"));
            USCS_Node* Nested = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Nested"));
            USCS_Node* Sibling = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Sibling"));
            if (!Root || !Pivot || !Child || !Nested || !Sibling) return false;
            SCS->AddNode(Root);
            Root->AddChildNode(Pivot, true);
            Pivot->AddChildNode(Child, true);
            Pivot->AddChildNode(Sibling, true);
            Child->AddChildNode(Nested, true);
            Cast<USceneComponent>(Pivot->ComponentTemplate.Get())->SetRelativeTransform(
                FTransform(FRotator(0, 15, 0), FVector(100, 20, 10)));
            Cast<USceneComponent>(Child->ComponentTemplate.Get())->SetRelativeLocation(FVector(30, 0, 0));
            Cast<USceneComponent>(Sibling->ComponentTemplate.Get())->SetRelativeLocation(FVector(-30, 0, 0));
            Cast<USceneComponent>(Nested->ComponentTemplate.Get())->SetRelativeLocation(FVector(10, 0, 5));
            FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
            return Test.TestNotNull(TEXT("Compiled authored test Blueprint"), BP->GeneratedClass.Get());
        }
        USCS_Node* Node(FName Name) const
        { return Blueprint.IsValid() ? Blueprint->SimpleConstructionScript->FindSCSNode(Name) : nullptr; }
        UOriginPivotComponent* Pivot() const
        { const USCS_Node* N = Node(TEXT("Pivot"));
          return N ? Cast<UOriginPivotComponent>(N->ComponentTemplate.Get()) : nullptr; }
        FTransform Relative(FName Name) const
        { const USCS_Node* N = Node(Name);
          const USceneComponent* C = N ? Cast<USceneComponent>(N->ComponentTemplate.Get()) : nullptr;
          return C ? C->GetRelativeTransform() : FTransform::Identity; }
    };

    struct FIsolatedTransactor
    {
        TStrongObjectPtr<UTransactor> Original;
        TStrongObjectPtr<UTransBuffer> PrivateBuffer;
        bool bInstalled = false;
        FIsolatedTransactor()
        {
            if (!GEditor || !GEditor->Trans || GEditor->Trans->IsActive()) return;
            Original.Reset(GEditor->Trans.Get());
            UTransBuffer* Buffer = Cast<UTransBuffer>(GEditor->CreateTrans());
            if (!Buffer) return;
            PrivateBuffer.Reset(Buffer);
            GEditor->Trans = Buffer;
            bInstalled = true;
        }
        ~FIsolatedTransactor()
        { if (bInstalled && GEditor) GEditor->Trans = Original.Get(); }
    };
}

#define ORIGIN_CALC_TEST(Type, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Type, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_CALC_TEST(FOriginCalcBounds, "Origin.Runtime.Calculation.BoundsCenterAndBottom")
bool FOriginCalcBounds::RunTest(const FString&)
{
    FTestInput F; FVector Out; FString Error;
    F.Rule.PositionMode = EOriginPivotMode::BoundsCenter;
    TestTrue(TEXT("Bounds Center"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Center correct"), Near(Out, FVector(100, 150, 30)));
    F.Rule.PositionMode = EOriginPivotMode::BottomCenter;
    TestTrue(TEXT("Bottom Center"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Bottom correct"), Near(Out, FVector(100, 150, -30)));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcPoints, "Origin.Runtime.Calculation.BoundsPointAxes")
bool FOriginCalcPoints::RunTest(const FString&)
{
    FTestInput F; FVector Out; FString Error;
    F.Rule.PositionMode = EOriginPivotMode::BoundsPoint;
    F.Rule.BoundsAxes = FIntVector(0, 2, 1);
    TestTrue(TEXT("Min X / Max Y / Center Z"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Point correct"), Near(Out, FVector(20, 260, 30)));
    F.Rule.BoundsAxes = FIntVector(4, 1, 1);
    Out = FVector(987, 654, 321);
    TestFalse(TEXT("Invalid bounds axis rejected"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Failure leaves output unchanged"), Near(Out, FVector(987, 654, 321)));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcAverage, "Origin.Runtime.Calculation.DirectChildAverage")
bool FOriginCalcAverage::RunTest(const FString&)
{
    FTestInput F; FVector Out; FString Error;
    F.Rule.PositionMode = EOriginPivotMode::AverageChildPivots;
    TestTrue(TEXT("Average available"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Direct child average"), Near(Out, FVector(100, 150, 40)));
    F.Input.DirectChildOrigins.Reset();
    TestFalse(TEXT("Empty average rejected"), F.Evaluate(Out, Error));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcSources, "Origin.Runtime.Calculation.SourceModeParity")
bool FOriginCalcSources::RunTest(const FString&)
{
    FTestInput F; FVector Out; FString Error;
    for (EOriginPivotMode Mode : {EOriginPivotMode::ChildActor,
        EOriginPivotMode::ComponentSocket, EOriginPivotMode::ReferenceActor})
    {
        F.Rule.PositionMode = Mode;
        TestTrue(*FString::Printf(TEXT("Source mode %d"), static_cast<int32>(Mode)), F.Evaluate(Out, Error));
        TestTrue(TEXT("Source coordinate"), Near(Out, FVector(700, 800, 900)));
        F.Input.bHasReferencePosition = false;
        Out = FVector(555, 666, 777);
        TestFalse(TEXT("Missing source rejected"), F.Evaluate(Out, Error));
        TestTrue(TEXT("Missing source does not replace coordinate"), Near(Out, FVector(555, 666, 777)));
        F.Input.bHasReferencePosition = true;
    }
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcAbsolute, "Origin.Runtime.Calculation.WorldAndCustom")
bool FOriginCalcAbsolute::RunTest(const FString&)
{
    FTestInput F; FVector Out; FString Error;
    F.Rule.PositionMode = EOriginPivotMode::WorldOrigin;
    TestTrue(TEXT("World origin source"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Zero world"), Near(Out, FVector::ZeroVector));
    F.Rule.PositionMode = EOriginPivotMode::Custom;
    F.Rule.CustomPosition = FVector(-123, 45, 11);
    TestTrue(TEXT("Custom source"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Custom preserved"), Near(Out, FVector(-123, 45, 11)));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcOffsets, "Origin.Runtime.Calculation.OffsetFrameParity")
bool FOriginCalcOffsets::RunTest(const FString&)
{
    FTestInput F; FVector Out; FString Error;
    F.Rule.PositionMode = EOriginPivotMode::Custom;
    F.Rule.CustomPosition = FVector(100, 100, 0);
    F.Rule.Offset = FVector(10, 0, 0);
    F.Rule.bLocalOffset = false;
    TestTrue(TEXT("World offset"), F.Evaluate(Out, Error));
    TestTrue(TEXT("World offset applied"), Near(Out, FVector(110, 100, 0)));
    F.Rule.bLocalOffset = true;
    TestTrue(TEXT("Local offset"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Rotation-only local offset"), Near(Out, FVector(100, 110, 0)));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcInvalid, "Origin.Runtime.Calculation.InvalidDataIsAtomic")
bool FOriginCalcInvalid::RunTest(const FString&)
{
    FTestInput F; FVector Out(1, 2, 3); FString Error;
    F.Rule.PositionMode = EOriginPivotMode::BoundsCenter;
    F.Input.AssemblyBounds = FBox(ForceInit);
    TestFalse(TEXT("No geometry rejected"), F.Evaluate(Out, Error));
    TestTrue(TEXT("Output stays unchanged"), Near(Out, FVector(1, 2, 3)));
    F.Rule.PositionMode = EOriginPivotMode::Custom;
    F.Rule.CustomPosition = FVector(std::numeric_limits<double>::quiet_NaN(), 0, 0);
    TestFalse(TEXT("NaN rejected"), F.Evaluate(Out, Error));
    TestTrue(TEXT("NaN leaves output alone"), Near(Out, FVector(1, 2, 3)));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcParentMath, "Origin.Runtime.Calculation.ParentFrameAverage")
bool FOriginCalcParentMath::RunTest(const FString&)
{
    const FTransform Pivot(FRotator(0, 90, 0), FVector(100, 100, 10));
    const TArray<FTransform> Children = {
        FTransform(FVector(10, 0, 0)), FTransform(FVector(-10, 0, 0)) };
    FVector Result;
    TestTrue(TEXT("Parent-frame mean"), FOriginPivotSnapMath::AverageChildOrigins(Pivot, Children, Result));
    TestTrue(TEXT("Center in parent frame"), Near(Result, FVector(100, 100, 10)));
    return true;
}

ORIGIN_CALC_TEST(FOriginCalcHostCandidates, "Origin.Editor.Calculation.ActorComponentCandidateParity")
bool FOriginCalcHostCandidates::RunTest(const FString&)
{
    FIsolatedTransactor Trans;
    if (!TestTrue(TEXT("Isolated Undo buffer"), Trans.bInstalled)) return false;
    FEditorCalculationFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    FTransform AnchorCandidate, ComponentCandidate;
    TestTrue(TEXT("Anchor average candidate"), FOriginAnchorAuthoring::ComputeSnapCandidate(
        F.Anchor, EOriginPivotSnapAction::AverageDirectChildren, AnchorCandidate, Reason));
    FOriginPivotDefinition Rule;
    Rule.PositionMode = EOriginPivotMode::AverageChildPivots;
    TestTrue(TEXT("Component average candidate"), FOriginPivotAuthoring::ComputeSourceCandidate(
        F.Pivot, Rule, ComponentCandidate, Reason));
    TestTrue(TEXT("Both use identical average"), Near(AnchorCandidate.GetLocation(), ComponentCandidate.GetLocation()));
    Rule.PositionMode = EOriginPivotMode::BoundsCenter;
    TestTrue(TEXT("Component bounds with scene-only descendants"),
        FOriginPivotAuthoring::ComputeSourceCandidate(F.Pivot, Rule, ComponentCandidate, Reason));
    TestTrue(TEXT("Component empty bounds equal root positions"),
        Near(ComponentCandidate.GetLocation(), FVector(200, 300, 70)));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcSourcePersistence, "Origin.Editor.Calculation.SourceRuleSlotIsolation")
bool FOriginCalcSourcePersistence::RunTest(const FString&)
{
    FIsolatedTransactor Trans;
    if (!TestTrue(TEXT("Isolated Undo buffer"), Trans.bInstalled)) return false;
    FEditorCalculationFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    TestTrue(TEXT("Capture slot"), F.Pivot->CreateOriginPivotSlot(TEXT("Before"), Reason));
    FTransform SavedPose;
    TestTrue(TEXT("Saved pose exists"), F.Pivot->GetOriginPivotSlotPose(TEXT("Before"), SavedPose));
    const FTransform OriginalSavedPose = SavedPose;
    FOriginPivotDefinition Rule = FOriginPivotAuthoring::ReadSourceDefinition(F.Pivot);
    Rule.PositionMode = EOriginPivotMode::Custom;
    Rule.CustomPosition = FVector(45, 60, 30);
    const FTransform BeforeA = F.SourceA->GetComponentTransform();
    TestTrue(TEXT("Save source only"), FOriginPivotAuthoring::SaveSourceDefinition(F.Pivot, Rule, Reason));
    TestTrue(TEXT("Source metadata saved"), FOriginPivotAuthoring::ReadSourceDefinition(F.Pivot).PositionMode == EOriginPivotMode::Custom);
    TestTrue(TEXT("Source save does not move child"), PoseNear(BeforeA, F.SourceA->GetComponentTransform()));
    TestTrue(TEXT("Source save does not overwrite captured pose"),
        F.Pivot->GetOriginPivotSlotPose(TEXT("Before"), SavedPose) &&
        PoseNear(SavedPose, OriginalSavedPose));
    TestTrue(TEXT("Recalculate applied"), FOriginPivotAuthoring::RecalculateSourcePivot(F.Pivot, Reason));
    TestTrue(TEXT("Pivot moved to custom position"), Near(F.Pivot->GetComponentLocation(), Rule.CustomPosition));
    TestTrue(TEXT("Children remain at world positions"), PoseNear(BeforeA, F.SourceA->GetComponentTransform()));
    return true;
}
ORIGIN_CALC_TEST(FOriginCalcHostSnaps, "Origin.Editor.Calculation.AnchorSnapAndUndo")
bool FOriginCalcHostSnaps::RunTest(const FString&)
{
    FIsolatedTransactor Trans;
    if (!TestTrue(TEXT("Isolated Undo buffer"), Trans.bInstalled)) return false;
    FEditorCalculationFixture F;
    if (!F.Setup(*this)) return false;
    FText Reason;
    const FTransform OriginalAnchor = F.Anchor->GetActorTransform();
    const FTransform OriginalChildA = F.ChildA->GetActorTransform();
    const FTransform OriginalChildB = F.ChildB->GetActorTransform();
    TestTrue(TEXT("Anchor snaps to average child origins"), FOriginAnchorAuthoring::SnapPivot(
        F.Anchor, EOriginPivotSnapAction::AverageDirectChildren, Reason));
    TestTrue(TEXT("Anchor midpoint"), Near(F.Anchor->GetActorLocation(), FVector(200, 300, 70)));
    TestTrue(TEXT("Child A preserved"), PoseNear(F.ChildA->GetActorTransform(), OriginalChildA));
    TestTrue(TEXT("Child B preserved"), PoseNear(F.ChildB->GetActorTransform(), OriginalChildB));
    TestTrue(TEXT("Undo snap"), Trans.PrivateBuffer->Undo(true));
    TestTrue(TEXT("Original pivot restored"), PoseNear(F.Anchor->GetActorTransform(), OriginalAnchor));
    TestTrue(TEXT("Child A preserved after Undo"), PoseNear(F.ChildA->GetActorTransform(), OriginalChildA));
    TestTrue(TEXT("Redo snap"), Trans.PrivateBuffer->Redo());
    TestTrue(TEXT("Midpoint after redo"), Near(F.Anchor->GetActorLocation(), FVector(200, 300, 70)));
    return true;
}

ORIGIN_CALC_TEST(FOriginCalcAuthoredSource, "Origin.Editor.Calculation.BlueprintSourceAndHierarchy")
bool FOriginCalcAuthoredSource::RunTest(const FString&)
{
    FIsolatedTransactor Trans;
    if (!TestTrue(TEXT("Isolated authoring Undo"), Trans.bInstalled)) return false;
    FAuthoredSourceFixture F;
    if (!F.Setup(*this)) return false;
    UOriginPivotComponent* Pivot = F.Pivot();
    if (!TestNotNull(TEXT("Authored pivot template"), Pivot)) return false;
    FOriginPivotDefinition Rule;
    Rule.PositionMode = EOriginPivotMode::ChildActor;
    Rule.ComponentName = TEXT("Child");
    FTransform Candidate;
    FText Reason;
    TestTrue(TEXT("Blueprint source resolves by SCS variable name"),
        FOriginPivotAuthoring::ComputeSourceCandidate(Pivot, Rule, Candidate, Reason));
    const FVector ExpectedSource = (F.Relative(TEXT("Child")) * F.Relative(TEXT("Pivot"))).GetLocation();
    TestTrue(TEXT("Authored source in parent-local frame"), Near(Candidate.GetLocation(), ExpectedSource));
    Rule.PositionMode = EOriginPivotMode::AverageChildPivots;
    TestTrue(TEXT("Authored direct child average"),
        FOriginPivotAuthoring::ComputeSourceCandidate(Pivot, Rule, Candidate, Reason));
    TestTrue(TEXT("Average source retains parent-local pivot origin"),
        Near(Candidate.GetLocation(), F.Relative(TEXT("Pivot")).GetLocation()));
    const FTransform ChildBefore = F.Relative(TEXT("Child")) * F.Relative(TEXT("Pivot"));
    const FTransform NestedBefore = F.Relative(TEXT("Nested")) * ChildBefore;
    Rule.PositionMode = EOriginPivotMode::Custom;
    Rule.CustomPosition = FVector(150, 80, 10);
    TestTrue(TEXT("Save authored source definition"),
        FOriginPivotAuthoring::SaveSourceDefinition(Pivot, Rule, Reason));
    TestTrue(TEXT("Recalculate authored source in one editor operation"),
        FOriginPivotAuthoring::RecalculateSourcePivot(Pivot, Reason));
    TestTrue(TEXT("Pivot authored relative position changed"), Near(F.Relative(TEXT("Pivot")).GetLocation(), Rule.CustomPosition));
    TestTrue(TEXT("Child authored parent-frame transform preserved"),
        PoseNear(F.Relative(TEXT("Child")) * F.Relative(TEXT("Pivot")), ChildBefore));
    TestTrue(TEXT("Nested authored parent-frame transform preserved"),
        PoseNear(F.Relative(TEXT("Nested")) * F.Relative(TEXT("Child")) * F.Relative(TEXT("Pivot")), NestedBefore));
    FKismetEditorUtilities::CompileBlueprint(F.Blueprint.Get(), EBlueprintCompileOptions::SkipGarbageCollection, nullptr);
    TestTrue(TEXT("Source definition survives Blueprint compilation"),
        FOriginPivotAuthoring::ReadSourceDefinition(F.Pivot()).PositionMode == EOriginPivotMode::Custom);
    return true;
}

ORIGIN_CALC_TEST(FOriginCalcAuthoredMissing, "Origin.Editor.Calculation.BlueprintMissingSourceAtomic")
bool FOriginCalcAuthoredMissing::RunTest(const FString&)
{
    FAuthoredSourceFixture F;
    if (!F.Setup(*this)) return false;
    UOriginPivotComponent* Pivot = F.Pivot();
    if (!TestNotNull(TEXT("Authored pivot"), Pivot)) return false;
    FOriginPivotDefinition Rule;
    Rule.PositionMode = EOriginPivotMode::ComponentSocket;
    Rule.ComponentName = TEXT("MissingComponent");
    Rule.SocketName = TEXT("LostSocket");
    const FTransform Before = F.Relative(TEXT("Pivot"));
    const FTransform ChildBefore = F.Relative(TEXT("Child"));
    FTransform Candidate;
    FText Reason;
    TestFalse(TEXT("Missing authored source rejected"),
        FOriginPivotAuthoring::ComputeSourceCandidate(Pivot, Rule, Candidate, Reason));
    TestTrue(TEXT("Missing source leaves authored pivot unchanged"), PoseNear(F.Relative(TEXT("Pivot")), Before));
    TestTrue(TEXT("Missing source leaves child unchanged"), PoseNear(F.Relative(TEXT("Child")), ChildBefore));
    return true;
}

#undef ORIGIN_CALC_TEST
#endif // WITH_DEV_AUTOMATION_TESTS
