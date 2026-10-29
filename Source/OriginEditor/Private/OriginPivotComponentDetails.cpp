// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponentDetails.h"

#include "OriginPivotComponent.h"
#include "OriginOperations.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/SBoxPanel.h"
#include "IDetailGroup.h"
#include "GameFramework/Actor.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "OriginPivotComponentDetails"

namespace
{
    struct FOriginPivotEditState
    {
        TWeakObjectPtr<UOriginPivotComponent> Component;
        FVector TranslationDelta = FVector::ZeroVector;
        // Displayed as Pitch/Yaw/Roll in degrees, composed in world space.
        FRotator RotationDelta = FRotator::ZeroRotator;

        bool CanEdit() const
        {
            const UOriginPivotComponent* Pivot = Component.Get();
            return Pivot && !Pivot->IsTemplate() && Pivot->IsRegistered() &&
                Pivot->GetOwner() && Pivot->GetWorld() &&
                Pivot->GetWorld()->WorldType == EWorldType::Editor &&
                Pivot->Mobility == EComponentMobility::Movable &&
                GEditor && !GEditor->PlayWorld;
        }

        FTransform MakeCandidate() const
        {
            const FTransform Current = Component->GetComponentTransform();
            FTransform Candidate = Current;
            Candidate.AddToTranslation(TranslationDelta);
            Candidate.SetRotation(RotationDelta.Quaternion() * Current.GetRotation());
            return Candidate;
        }

        bool HasDelta() const
        {
            return !TranslationDelta.IsNearlyZero() || !RotationDelta.IsNearlyZero();
        }

        FReply Apply()
        {
            UOriginPivotComponent* Pivot = Component.Get();
            if (!CanEdit() || !HasDelta()) return FReply::Handled();

            const FTransform Candidate = MakeCandidate();
            FText Failure;
            const EOriginPivotResult Validation = Pivot->CanSetOriginPivotWorldTransform(Candidate, Failure);
            if (Validation == EOriginPivotResult::NoChange) return FReply::Handled();
            if (Validation != EOriginPivotResult::Success)
            {
                Origin::Notify(Failure);
                return FReply::Handled();
            }

            FScopedTransaction Transaction(LOCTEXT("EditPivotTransaction", "Origin: Edit Pivot (Preserve Children)"));
            // Record all affected objects before touching the native attachment tree.
            // This includes the owning Actors of externally attached components.
            TArray<USceneComponent*> Descendants;
            Pivot->GetChildrenComponents(true, Descendants);
            TSet<AActor*> Owners;
            if (AActor* Owner = Pivot->GetOwner()) Owners.Add(Owner);
            for (USceneComponent* Child : Descendants)
            {
                if (IsValid(Child))
                {
                    Child->Modify();
                    if (AActor* Owner = Child->GetOwner()) Owners.Add(Owner);
                }
            }
            for (AActor* Owner : Owners) if (IsValid(Owner)) Owner->Modify();
            Pivot->Modify();

            const EOriginPivotResult Result = Pivot->SetOriginPivotWorldTransform(Candidate, Failure);
            if (Result != EOriginPivotResult::Success)
            {
                // FOriginPivotCore restores world transforms when preservation fails.
                // Cancel the editor transaction so the rejected operation is not undoable.
                Transaction.Cancel();
                if (Result != EOriginPivotResult::NoChange) Origin::Notify(Failure);
                return FReply::Handled();
            }
            Pivot->MarkPackageDirty();
            for (AActor* Owner : Owners) if (IsValid(Owner)) Owner->MarkPackageDirty();
            TranslationDelta = FVector::ZeroVector;
            RotationDelta = FRotator::ZeroRotator;
            GEditor->RedrawLevelEditingViewports();
            return FReply::Handled();
        }
    };

    TSharedRef<SWidget> MakeDeltaFields(const TSharedRef<FOriginPivotEditState>& State, bool bRotation)
    {
        TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
        for (int32 Index = 0; Index < 3; ++Index)
        {
            const TCHAR* AxisLabel = bRotation
                ? (Index == 0 ? TEXT("Pitch") : Index == 1 ? TEXT("Yaw") : TEXT("Roll"))
                : (Index == 0 ? TEXT("X") : Index == 1 ? TEXT("Y") : TEXT("Z"));
            Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(Index == 0 ? 0.0f : 5.0f, 0.0f, 2.0f, 0.0f)
            [SNew(STextBlock).Text(FText::FromString(AxisLabel)).Font(FAppStyle::Get().GetFontStyle(TEXT("SmallFont")))];
            Row->AddSlot().FillWidth(1.0f)
            [SNew(SNumericEntryBox<double>)
                .MinDesiredValueWidth(42.0f)
                .AllowSpin(false)
                .IsEnabled_Lambda([State] { return State->CanEdit(); })
                .Value_Lambda([State, bRotation, Index]() -> TOptional<double>
                {
                    if (bRotation)
                    {
                        return Index == 0 ? State->RotationDelta.Pitch : Index == 1 ? State->RotationDelta.Yaw : State->RotationDelta.Roll;
                    }
                    return State->TranslationDelta[Index];
                })
                .OnValueChanged_Lambda([State, bRotation, Index](double Value)
                {
                    if (bRotation)
                    {
                        if (Index == 0) State->RotationDelta.Pitch = Value;
                        else if (Index == 1) State->RotationDelta.Yaw = Value;
                        else State->RotationDelta.Roll = Value;
                    }
                    else State->TranslationDelta[Index] = Value;
                })];
        }
        return Row;
    }
}

void FOriginPivotComponentDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
    // Deliberately hide the implementation-facing TArray (Index [0], Definition,
    // etc.). This data remains serialized and Blueprint-accessible.
    Builder.HideProperty(GET_MEMBER_NAME_CHECKED(UOriginPivotComponent, OriginPivotConfiguration));

    TArray<TWeakObjectPtr<UObject>> Objects;
    Builder.GetObjectsBeingCustomized(Objects);
    IDetailCategoryBuilder& Category = Builder.EditCategory(TEXT("Origin"), LOCTEXT("Category", "Origin"), ECategoryPriority::Important);
    if (Objects.Num() != 1)
    {
        Category.AddCustomRow(LOCTEXT("MultiSelection", "Origin Pivot"))
            .WholeRowContent()[SNew(STextBlock)
                .Text(LOCTEXT("SingleSelection", "Select one Origin Pivot Component to edit its pivot."))
                .AutoWrapText(true)];
        return;
    }
    UOriginPivotComponent* Pivot = Cast<UOriginPivotComponent>(Objects[0].Get());
    if (!Pivot) return;

    const auto State = MakeShared<FOriginPivotEditState>();
    State->Component = Pivot;
    const TWeakObjectPtr<UOriginPivotComponent> WeakPivot(Pivot);

    Category.AddCustomRow(LOCTEXT("ActiveSlotSearch", "Active Pivot"))
        .NameContent()[SNew(STextBlock)
            .Text(LOCTEXT("ActivePivot", "Active Pivot"))
            .ToolTipText(LOCTEXT("ActivePivotTip", "The currently selected named pivot definition. Slot editing and activation arrive in a later 2.0 alpha."))]
        .ValueContent().MinDesiredWidth(240.0f)
        [SNew(STextBlock).Text_Lambda([WeakPivot]
        {
            return WeakPivot.IsValid() ? FText::FromName(WeakPivot->OriginPivotConfiguration.ActiveSlotName) : FText::GetEmpty();
        })];

    Category.AddCustomRow(LOCTEXT("PivotDescriptionSearch", "Pivot help"))
        .WholeRowContent()
        [SNew(STextBlock)
            .Text(LOCTEXT("PivotHelp", "Normal component movement moves the assembly. Editing the pivot below moves its control point while preserving attached world transforms."))
            .AutoWrapText(true)];

    IDetailGroup& Edit = Category.AddGroup(TEXT("OriginPivotEdit"), LOCTEXT("EditPivotGroup", "Edit Pivot"), false, true);
    Edit.AddWidgetRow()
        .NameContent()[SNew(STextBlock)
            .Text(LOCTEXT("PositionDelta", "Move Pivot (cm)"))
            .ToolTipText(LOCTEXT("PositionDeltaTip", "World-space translation applied to the pivot, not the assembly."))]
        .ValueContent().MinDesiredWidth(240.0f)[MakeDeltaFields(State, false)];
    Edit.AddWidgetRow()
        .NameContent()[SNew(STextBlock)
            .Text(LOCTEXT("RotationDelta", "Rotate Pivot (degrees)"))
            .ToolTipText(LOCTEXT("RotationDeltaTip", "World-space pitch, yaw and roll applied to the pivot without rotating its attached content."))]
        .ValueContent().MinDesiredWidth(240.0f)[MakeDeltaFields(State, true)];
    Edit.AddWidgetRow().WholeRowContent()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 3, 6, 0)
            [SNew(SButton)
                .Text(LOCTEXT("Apply", "Apply Pivot Edit"))
                .ToolTipText(LOCTEXT("ApplyTip", "Apply the offset as a single Undo transaction. Attached Actors and Components retain their world transforms."))
                .IsEnabled_Lambda([State] { return State->CanEdit() && State->HasDelta(); })
                .OnClicked_Lambda([State] { return State->Apply(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 3, 0, 0)
            [SNew(SButton)
                .Text(LOCTEXT("Reset", "Clear"))
                .IsEnabled_Lambda([State] { return State->HasDelta(); })
                .OnClicked_Lambda([State]
                {
                    State->TranslationDelta = FVector::ZeroVector;
                    State->RotationDelta = FRotator::ZeroRotator;
                    return FReply::Handled();
                })]];
    Edit.AddWidgetRow().WholeRowContent()
        [SNew(STextBlock)
            .Text_Lambda([State]
            {
                return State->CanEdit()
                    ? LOCTEXT("DeltaHelp", "Applies to placed, movable instances. Pivot scale is unchanged. Native snapping and viewport gizmo editing are planned separately.")
                    : LOCTEXT("DeltaUnavailable", "Edit Pivot is available for placed, movable, registered components in an editor level. Blueprint defaults and preview components are not yet supported.");
            })
            .AutoWrapText(true)];
}

#undef LOCTEXT_NAMESPACE
