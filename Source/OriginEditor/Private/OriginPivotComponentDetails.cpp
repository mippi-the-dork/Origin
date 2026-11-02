// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponentDetails.h"

#include "OriginPivotComponent.h"
#include "OriginOperations.h"
#include "OriginPivotEditSession.h"
#include "OriginBlueprintPivotEdit.h"
#include "OriginPivotViewportMode.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Selection.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"
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
            if (!Pivot || !GEditor || GEditor->PlayWorld) return false;
            if (Pivot->IsTemplate())
            {
                UBlueprint* BP = nullptr;
                USCS_Node* Node = nullptr;
                return OriginBlueprintPivot::Resolve(const_cast<UOriginPivotComponent*>(Pivot), BP, Node);
            }
            return Pivot->IsRegistered() && Pivot->GetOwner() && Pivot->GetWorld() &&
                Pivot->GetWorld()->WorldType == EWorldType::Editor &&
                Pivot->Mobility == EComponentMobility::Movable;
        }

        FTransform MakeCandidate() const
        {
            const FTransform Current = Component->IsTemplate()
                ? Component->GetRelativeTransform() : Component->GetComponentTransform();
            FTransform Candidate = Current;
            Candidate.AddToTranslation(TranslationDelta);
            Candidate.SetRotation(RotationDelta.Quaternion() * Current.GetRotation());
            return Candidate;
        }

        bool GetViewportDeltas(FVector& Translation, FRotator& Rotation) const
        {
            return FOriginPivotViewportMode::GetLiveEditDeltas(Component.Get(), Translation, Rotation);
        }

        bool CanEditNumeric() const
        {
            return CanEdit() && !FOriginPivotViewportMode::IsEditing(Component.Get());
        }

        bool HasDelta() const
        {
            return !TranslationDelta.IsNearlyZero() || !RotationDelta.IsNearlyZero();
        }

        FReply Apply()
        {
            UOriginPivotComponent* Pivot = Component.Get();
            if (!CanEditNumeric() || !HasDelta()) return FReply::Handled();

            const FTransform Candidate = MakeCandidate();
            FText Failure;
            if (Pivot->IsTemplate())
            {
                OriginBlueprintPivot::FPlan Plan;
                if (!OriginBlueprintPivot::Plan(Pivot, Candidate, Plan, Failure))
                {
                    Origin::Notify(Failure);
                    return FReply::Handled();
                }
                FOriginBlueprintPivotEditSession Session(Pivot);
                if (!Session.Apply(Candidate, Failure))
                {
                    Origin::Notify(Failure);
                    Session.Cancel();
                    return FReply::Handled();
                }
                Session.Finish();
                TranslationDelta = FVector::ZeroVector;
                RotationDelta = FRotator::ZeroRotator;
                return FReply::Handled();
            }
            const EOriginPivotResult Validation = Pivot->CanSetOriginPivotWorldTransform(Candidate, Failure);
            if (Validation == EOriginPivotResult::NoChange) return FReply::Handled();
            if (Validation != EOriginPivotResult::Success)
            {
                Origin::Notify(Failure);
                return FReply::Handled();
            }

            FOriginPivotEditSession Session(Pivot);
            const EOriginPivotResult Result = Session.Apply(Candidate, Failure);
            if (Result != EOriginPivotResult::Success)
            {
                Session.Cancel();
                if (Result != EOriginPivotResult::NoChange) Origin::Notify(Failure);
                return FReply::Handled();
            }
            Session.Finish();
            TranslationDelta = FVector::ZeroVector;
            RotationDelta = FRotator::ZeroRotator;

            // SetWorldTransform updates the component, but the Level Editor caches
            // its widget pivot separately. A redraw alone can leave the translate
            // gizmo at its previous location until the component is reselected.
            // Ask Unreal to recalculate the widget for the existing selection;
            // never change the selection or the viewport pivot for unrelated actors.
            const USelection* SelectedComponents = GEditor->GetSelectedComponents();
            if (GUnrealEd && SelectedComponents && SelectedComponents->IsSelected(Pivot))
            {
                GUnrealEd->UpdatePivotLocationForSelection(false);
            }
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
                .IsEnabled_Lambda([State] { return State->CanEditNumeric(); })
                .Value_Lambda([State, bRotation, Index]() -> TOptional<double>
                {
                    FVector LiveTranslation;
                    FRotator LiveRotation;
                    if (State->GetViewportDeltas(LiveTranslation, LiveRotation))
                    {
                        if (bRotation)
                            return Index == 0 ? LiveRotation.Pitch : Index == 1 ? LiveRotation.Yaw : LiveRotation.Roll;
                        return LiveTranslation[Index];
                    }
                    if (bRotation)
                    {
                        return Index == 0 ? State->RotationDelta.Pitch : Index == 1 ? State->RotationDelta.Yaw : State->RotationDelta.Roll;
                    }
                    return State->TranslationDelta[Index];
                })
                .OnValueChanged_Lambda([State, bRotation, Index](double Value)
                {
                    if (!State->CanEditNumeric()) return;
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
    // Blueprint Details can expose either the SCS template or its preview
    // counterpart. In either case edit the source template, never preview-only.
    UOriginPivotComponent* AuthoredTemplate = nullptr;
    UBlueprint* OwningBlueprint = nullptr;
    USCS_Node* AuthoredNode = nullptr;
    State->Component = OriginBlueprintPivot::ResolveSelection(
        Pivot, AuthoredTemplate, OwningBlueprint, AuthoredNode) ? AuthoredTemplate : Pivot;
    const TWeakObjectPtr<UOriginPivotComponent> WeakPivot(State->Component);

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
    Edit.AddWidgetRow().WholeRowContent()
        [SNew(SButton)
            .Text_Lambda([WeakPivot]
            {
                return FOriginPivotViewportMode::IsEditing(WeakPivot.Get())
                    ? LOCTEXT("FinishViewportPivot", "Finish Viewport Pivot Editing")
                    : LOCTEXT("BeginViewportPivot", "Edit Pivot in Viewport");
            })
            .ToolTipText(LOCTEXT("ViewportPivotTip", "Uses native Translate (W) and Rotate (E) widgets. In Blueprints this edits authored SCS templates (not just the preview); in levels it preserves attached actors and components. One Undo step per drag. Scale is unavailable."))
            .IsEnabled_Lambda([State] { return State->CanEdit(); })
            .OnClicked_Lambda([WeakPivot, State]
            {
                if (UOriginPivotComponent* Component = WeakPivot.Get())
                {
                    // Pending typed offsets must never be carried into an active
                    // viewport edit and accidentally re-applied afterward.
                    State->TranslationDelta = FVector::ZeroVector;
                    State->RotationDelta = FRotator::ZeroRotator;
                    FOriginPivotViewportMode::Toggle(Component);
                }
                return FReply::Handled();
            })];
    Edit.AddWidgetRow().WholeRowContent()
        [SNew(STextBlock)
            .Text_Lambda([WeakPivot]
            {
                return FOriginPivotViewportMode::IsEditing(WeakPivot.Get())
                    ? LOCTEXT("ViewportActiveHint", "Viewport editing active. The numbers below show offsets already applied since entering this mode (read-only). W: move, E: rotate. Esc: cancel current drag and exit.")
                    : LOCTEXT("ViewportInactiveHint", "Select this Origin Pivot component in a level or Blueprint Components tree to edit its pivot directly.");
            })
            .AutoWrapText(true)];
    Edit.AddWidgetRow()
        .NameContent()[SNew(STextBlock)
            .Text(LOCTEXT("PositionDelta", "Move Pivot (cm)"))
            .ToolTipText(LOCTEXT("PositionDeltaTip", "Pivot translation offset (world space for placed components, parent-relative for Blueprint templates)."))]
        .ValueContent().MinDesiredWidth(240.0f)[MakeDeltaFields(State, false)];
    Edit.AddWidgetRow()
        .NameContent()[SNew(STextBlock)
            .Text(LOCTEXT("RotationDelta", "Rotate Pivot (degrees)"))
            .ToolTipText(LOCTEXT("RotationDeltaTip", "Pivot rotation offset (world space for placed components, parent-relative for Blueprint templates)."))]
        .ValueContent().MinDesiredWidth(240.0f)[MakeDeltaFields(State, true)];
    Edit.AddWidgetRow().WholeRowContent()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 3, 6, 0)
            [SNew(SButton)
                .Text(LOCTEXT("Apply", "Apply Pivot Edit"))
                .ToolTipText(LOCTEXT("ApplyTip", "Apply the offset as a single Undo transaction. Attached Actors and Components retain their world transforms."))
                .IsEnabled_Lambda([State] { return State->CanEditNumeric() && State->HasDelta(); })
                .OnClicked_Lambda([State] { return State->Apply(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 3, 0, 0)
            [SNew(SButton)
                .Text(LOCTEXT("Reset", "Clear"))
                .IsEnabled_Lambda([State] { return State->CanEditNumeric() && State->HasDelta(); })
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
                    ? LOCTEXT("DeltaHelp", "Numeric offsets apply only when you click Apply. During viewport editing they display live, already-applied offsets and cannot be entered or applied twice. Pivot scale remains unchanged.")
                    : LOCTEXT("DeltaUnavailable", "Edit Pivot supports placed, movable components and Blueprint-owned SCS templates. Inherited/native component defaults, unsupported scales, and Play In Editor are intentionally excluded.");
            })
            .AutoWrapText(true)];
}

#undef LOCTEXT_NAMESPACE
