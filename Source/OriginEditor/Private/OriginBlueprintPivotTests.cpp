// Copyright Mippithedork 2026, Inc. All Rights Reserved.
// Native Automation Framework tests. These do not create or compile a Blueprint.
// UE 5.8's SCS APIs need an engine-initialized generated class; never construct
// a bare UBlueprint and manually substitute a SimpleConstructionScript fixture.
#include "OriginBlueprintPivotEdit.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace
{
    bool Near(const FTransform& A, const FTransform& B, double Tolerance = 1.e-3)
    {
        return A.Equals(B, Tolerance);
    }
}

#define ORIGIN_BP_MATH_TEST(Class, Name) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Name, \
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ORIGIN_BP_MATH_TEST(FOriginBPTranslationMathTest, "Origin.Editor.Blueprint.Math.TranslationPreservesChild")
bool FOriginBPTranslationMathTest::RunTest(const FString& Parameters)
{
    const FTransform PivotBefore(FRotator(0, 15, 0), FVector(45, 20, 5));
    const FTransform ChildBefore(FRotator(0, -12, 0), FVector(110, 20, -30));
    const FTransform ChildParentBefore = ChildBefore * PivotBefore;
    FTransform PivotAfter = PivotBefore;
    PivotAfter.AddToTranslation(FVector(100, -40, 12));
    FTransform ChildAfter = ChildBefore;
    if (!TestTrue(TEXT("Blueprint direct-child translation is compensable"),
        OriginBlueprintPivot::CompensateChild(PivotBefore, PivotAfter, ChildBefore, ChildAfter))) return false;
    TestTrue(TEXT("Child transform in pivot-parent space unchanged"),
        Near(ChildAfter * PivotAfter, ChildParentBefore));
    TestTrue(TEXT("Original child input unchanged"),
        Near(ChildBefore, FTransform(FRotator(0, -12, 0), FVector(110, 20, -30))));
    return true;
}

ORIGIN_BP_MATH_TEST(FOriginBPRotationMathTest, "Origin.Editor.Blueprint.Math.RotationPreservesNested")
bool FOriginBPRotationMathTest::RunTest(const FString& Parameters)
{
    const FTransform PivotBefore(FRotator(0, 15, 0), FVector(45, 20, 5));
    const FTransform ChildBefore(FRotator(0, -12, 0), FVector(110, 20, -30));
    const FTransform GrandchildBefore(FRotator(2, 5, 0), FVector(12, 34, 56));
    const FTransform GrandchildParentBefore = GrandchildBefore * ChildBefore * PivotBefore;
    FTransform PivotAfter = PivotBefore;
    PivotAfter.SetRotation(FRotator(0, 70, 0).Quaternion() * PivotBefore.GetRotation());
    FTransform ChildAfter;
    if (!TestTrue(TEXT("Blueprint direct-child rotation is compensable"),
        OriginBlueprintPivot::CompensateChild(PivotBefore, PivotAfter, ChildBefore, ChildAfter))) return false;
    TestTrue(TEXT("Nested child transform in pivot-parent space unchanged"),
        Near(GrandchildBefore * ChildAfter * PivotAfter, GrandchildParentBefore));
    return true;
}

ORIGIN_BP_MATH_TEST(FOriginBPMultiDeltaMathTest, "Origin.Editor.Blueprint.Math.MultiDeltaRoundTrip")
bool FOriginBPMultiDeltaMathTest::RunTest(const FString& Parameters)
{
    const FTransform PivotInitial(FRotator(0, 10, 0), FVector(5, 10, 15));
    const FTransform ChildInitial(FRotator(0, -20, 0), FVector(150, 0, 0));
    FTransform PivotCurrent = PivotInitial;
    FTransform ChildCurrent = ChildInitial;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        FTransform Next = PivotCurrent;
        Next.AddToTranslation(FVector(10, 5, -2));
        Next.SetRotation(FRotator(0, 10, 0).Quaternion() * Next.GetRotation());
        FTransform ChildNext;
        if (!TestTrue(TEXT("Each drag delta compensable"),
            OriginBlueprintPivot::CompensateChild(PivotCurrent, Next, ChildCurrent, ChildNext))) return false;
        PivotCurrent = Next;
        ChildCurrent = ChildNext;
        TestTrue(TEXT("Preserved world-equivalent frame after each segment"),
            Near(ChildCurrent * PivotCurrent, ChildInitial * PivotInitial));
    }
    FTransform RestoredChild;
    if (!TestTrue(TEXT("Inverse to starting pivot is compensable"),
        OriginBlueprintPivot::CompensateChild(PivotCurrent, PivotInitial, ChildCurrent, RestoredChild))) return false;
    TestTrue(TEXT("Round trip reproduces child local transform"), Near(RestoredChild, ChildInitial));
    return true;
}

ORIGIN_BP_MATH_TEST(FOriginBPScaleMathTest, "Origin.Editor.Blueprint.Math.RejectsNonUniformScale")
bool FOriginBPScaleMathTest::RunTest(const FString& Parameters)
{
    const FTransform PivotBefore = FTransform::Identity;
    FTransform PivotAfter = PivotBefore;
    PivotAfter.AddToTranslation(FVector(100, 0, 0));
    const FTransform UnsupportedChild(FQuat::Identity, FVector(50, 0, 0), FVector(2, 1, 1));
    FTransform After = UnsupportedChild;
    TestFalse(TEXT("Reject non-uniform child scale"),
        OriginBlueprintPivot::CompensateChild(PivotBefore, PivotAfter, UnsupportedChild, After));
    TestTrue(TEXT("Rejected compensation leaves output unchanged"), Near(After, UnsupportedChild));
    PivotAfter.SetScale3D(FVector(2, 2, 2));
    After = FTransform::Identity;
    TestFalse(TEXT("Reject a pivot scale edit"),
        OriginBlueprintPivot::CompensateChild(PivotBefore, PivotAfter, FTransform::Identity, After));
    return true;
}
ORIGIN_BP_MATH_TEST(FOriginBPNativeViewportReconciliationMathTest, "Origin.Editor.Blueprint.Math.NativeViewportDriftReconciliation")
bool FOriginBPNativeViewportReconciliationMathTest::RunTest(const FString& Parameters)
{
    // Simulate the Blueprint editor moving the selected component *natively*
    // before Origin receives an InputDelta callback. Original child relative
    // transforms are unchanged, so the entire child visually moves.
    const FTransform InitialPivot(FRotator(0, 10, 0), FVector(20, 5, 0));
    const FTransform OriginalChild(FRotator(0, -10, 0), FVector(150, 0, 0));
    const FTransform OriginalChildParentFrame = OriginalChild * InitialPivot;
    FTransform NativeMovedPivot = InitialPivot;
    NativeMovedPivot.AddToTranslation(FVector(75, -20, 0));
    const FTransform UncompensatedChildParentFrame = OriginalChild * NativeMovedPivot;
    TestFalse(TEXT("Native component move without compensation displaces child"),
        Near(UncompensatedChildParentFrame, OriginalChildParentFrame));
    FTransform CorrectedChild;
    if (!TestTrue(TEXT("Original drag-start child snapshot compensates the native move"),
        OriginBlueprintPivot::CompensateChild(InitialPivot, NativeMovedPivot, OriginalChild, CorrectedChild))) return false;
    TestTrue(TEXT("Corrected child remains fixed after native component movement"),
        Near(CorrectedChild * NativeMovedPivot, OriginalChildParentFrame));
    // Each successive preview movement must preserve the SAME baseline, not
    // accumulate corrections from whatever the preview already moved.
    NativeMovedPivot.AddToTranslation(FVector(-25, 80, 4));
    FTransform CorrectedAgain;
    if (!TestTrue(TEXT("Second native delta reconciles from immutable original"),
        OriginBlueprintPivot::CompensateChild(InitialPivot, NativeMovedPivot, OriginalChild, CorrectedAgain))) return false;
    TestTrue(TEXT("No cumulative drift over multiple native deltas"),
        Near(CorrectedAgain * NativeMovedPivot, OriginalChildParentFrame));
    return true;
}

#undef ORIGIN_BP_MATH_TEST
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOriginBPPreviewTransformContractTest,
    "Origin.Editor.Blueprint.PreviewContract.RelativeTransformValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOriginBPPreviewTransformContractTest::RunTest(const FString& Parameters)
{
    const FTransform Authored(FRotator(0, 25, 0), FVector(100, 20, 40));
    const FTransform IdenticalPreview = Authored;
    TestTrue(TEXT("Matching authored and preview relative transforms are accepted"),
        OriginBlueprintPivot::IsUnmodifiedPreviewTransform(Authored, IdenticalPreview));
    FTransform ConstructionScriptOverride = IdenticalPreview;
    ConstructionScriptOverride.AddToTranslation(FVector(10, 0, 0));
    TestFalse(TEXT("Construction Script relative offset is rejected"),
        OriginBlueprintPivot::IsUnmodifiedPreviewTransform(Authored, ConstructionScriptOverride));
    return true;
}
#endif
