// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotComponentDetails.h"

#include "OriginPivotComponent.h"
#include "OriginOperations.h"
#include "OriginPivotAuthoring.h"
#include "OriginHierarchyAuthoring.h"
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
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SComboButton.h"
#include "PropertyCustomizationHelpers.h"
#include "AssetRegistry/AssetData.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/SBoxPanel.h"
#include "IDetailGroup.h"
#include "GameFramework/Actor.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "OriginPivotComponentDetails"

namespace
{
    const TCHAR* SourceModeLabel(EOriginPivotMode Mode)
    {
        switch (Mode)
        {
        case EOriginPivotMode::BoundsCenter: return TEXT("Bounds Center");
        case EOriginPivotMode::AverageChildPivots: return TEXT("Average Child Origins");
        case EOriginPivotMode::BottomCenter: return TEXT("Bottom Center");
        case EOriginPivotMode::BoundsPoint: return TEXT("Bounds Point");
        case EOriginPivotMode::ChildActor: return TEXT("Child Component");
        case EOriginPivotMode::ComponentSocket: return TEXT("Component / Socket");
        case EOriginPivotMode::ReferenceActor: return TEXT("Reference Actor");
        case EOriginPivotMode::WorldOrigin: return TEXT("World Origin");
        case EOriginPivotMode::Custom: return TEXT("Custom Position");
        default: return TEXT("Unknown");
        }
    }

    struct FOriginPivotEditState : public TSharedFromThis<FOriginPivotEditState>
    {
        TWeakObjectPtr<UOriginPivotComponent> Component;
        FVector TranslationDelta = FVector::ZeroVector;
        // Displayed as Pitch/Yaw/Roll in degrees, composed in world space.
        FRotator RotationDelta = FRotator::ZeroRotator;
        FName SelectedSlotName = TEXT("Default");
        FString ProposedSlotName = TEXT("NewPivot");
        TArray<TSharedPtr<FName>> SlotOptions;
        TWeakPtr<SComboBox<TSharedPtr<FName>>> SlotCombo;
        FName SelectedSibling = NAME_None;
        FName SelectedChild = NAME_None;
        FName SelectedTransferTarget = NAME_None;
        TArray<TSharedPtr<FName>> TransferTargetOptions;
        TWeakPtr<SComboBox<TSharedPtr<FName>>> TransferTargetCombo;
        TSet<FName> BatchSiblings;
        TSet<FName> BatchChildren;
        TArray<TSharedPtr<FName>> SiblingOptions;
        TArray<TSharedPtr<FName>> ChildOptions;
        TWeakPtr<SComboBox<TSharedPtr<FName>>> SiblingCombo;
        TWeakPtr<SComboBox<TSharedPtr<FName>>> ChildCombo;

        void RefreshHierarchyChoices()
        {
            TArray<FName> Siblings, Children, Targets;
            FOriginHierarchyAuthoring::ListSiblingsAndChildren(Component.Get(), Siblings, Children);
            FOriginHierarchyAuthoring::ListTransferTargets(Component.Get(), Targets);
            SiblingOptions.Reset();
            ChildOptions.Reset();
            TransferTargetOptions.Reset();
            for (FName Name : Siblings) SiblingOptions.Add(MakeShared<FName>(Name));
            for (FName Name : Children) ChildOptions.Add(MakeShared<FName>(Name));
            for (FName Name : Targets) TransferTargetOptions.Add(MakeShared<FName>(Name));
            if (!Targets.Contains(SelectedTransferTarget))
                SelectedTransferTarget = Targets.IsEmpty() ? NAME_None : Targets[0];
            if (!Siblings.Contains(SelectedSibling))
                SelectedSibling = Siblings.IsEmpty() ? NAME_None : Siblings[0];
            if (!Children.Contains(SelectedChild))
                SelectedChild = Children.IsEmpty() ? NAME_None : Children[0];
            for (auto It = BatchSiblings.CreateIterator(); It; ++It)
                if (!Siblings.Contains(*It)) It.RemoveCurrent();
            for (auto It = BatchChildren.CreateIterator(); It; ++It)
                if (!Children.Contains(*It)) It.RemoveCurrent();
            if (auto Combo = SiblingCombo.Pin()) Combo->RefreshOptions();
            if (auto Combo = ChildCombo.Pin()) Combo->RefreshOptions();
            if (auto Combo = TransferTargetCombo.Pin()) Combo->RefreshOptions();
        }

        FReply Restructure(int32 Action)
        {
            UOriginPivotComponent* Pivot = Component.Get();
            if (!FOriginHierarchyAuthoring::CanEdit(Pivot)) return FReply::Handled();
            FText Reason;
            bool bOk = false;
            if (Action == 0 && !SelectedSibling.IsNone())
                bOk = FOriginHierarchyAuthoring::AdoptSibling(Pivot, SelectedSibling, Reason);
            else if (Action == 1 && !SelectedChild.IsNone())
                bOk = FOriginHierarchyAuthoring::PromoteChild(Pivot, SelectedChild, Reason);
            else if (Action == 2 && !SelectedChild.IsNone())
            {
                FName NewVariable;
                bOk = FOriginHierarchyAuthoring::InsertPivotAboveChild(Pivot, SelectedChild, NewVariable, Reason);
            }
            if (!bOk)
            {
                if (Reason.IsEmpty()) Reason = FText::FromString(TEXT("Select an authored component from the relevant list."));
                Origin::Notify(Reason);
            }
            else RefreshHierarchyChoices();
            return FReply::Handled();
        }


        TSharedRef<SWidget> MakeBatchSelectionMenu(bool bSiblings)
        {
            // Menu entries may outlive a Details rebuild. Never capture a raw
            // authoring-state pointer in a deferred Slate menu delegate.
            const TWeakPtr<FOriginPivotEditState> WeakSelf = AsShared();
            // Keep the menu open for multi-select; menu should only close when
            // the user clicks away, not after checking each component.
            FMenuBuilder Menu(false, nullptr);
            const TArray<TSharedPtr<FName>>& Options = bSiblings ? SiblingOptions : ChildOptions;
            Menu.AddMenuEntry(LOCTEXT("BatchSelectAll", "Select All Eligible"),
                LOCTEXT("BatchSelectAllTip", "Include every currently eligible authored component."),
                FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([WeakSelf, bSiblings]
                {
                    if (const TSharedPtr<FOriginPivotEditState> State = WeakSelf.Pin())
                    {
                        TSet<FName>& Selection = bSiblings ? State->BatchSiblings : State->BatchChildren;
                        const auto& Candidates = bSiblings ? State->SiblingOptions : State->ChildOptions;
                        for (const TSharedPtr<FName>& Candidate : Candidates)
                            if (Candidate.IsValid()) Selection.Add(*Candidate);
                    }
                })));
            Menu.AddMenuEntry(LOCTEXT("BatchClearAll", "Clear Selection"),
                LOCTEXT("BatchClearAllTip", "Remove every component from the current batch selection."),
                FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([WeakSelf, bSiblings]
                {
                    if (const TSharedPtr<FOriginPivotEditState> State = WeakSelf.Pin())
                        (bSiblings ? State->BatchSiblings : State->BatchChildren).Reset();
                })));
            Menu.AddMenuSeparator();
            for (const TSharedPtr<FName>& Option : Options)
            {
                if (!Option.IsValid()) continue;
                const FName Name = *Option;
                Menu.AddMenuEntry(FText::FromName(Name),
                    LOCTEXT("BatchChooseTip", "Toggle this authored component in the atomic batch operation."),
                    FSlateIcon(), FUIAction(
                        FExecuteAction::CreateLambda([WeakSelf, bSiblings, Name]
                        {
                            if (const TSharedPtr<FOriginPivotEditState> State = WeakSelf.Pin())
                            {
                                TSet<FName>& Selected = bSiblings ? State->BatchSiblings : State->BatchChildren;
                                if (Selected.Contains(Name)) Selected.Remove(Name);
                                else Selected.Add(Name);
                            }
                        }),
                        FCanExecuteAction(),
                        FIsActionChecked::CreateLambda([WeakSelf, bSiblings, Name]
                        {
                            if (const TSharedPtr<FOriginPivotEditState> State = WeakSelf.Pin())
                                return (bSiblings ? State->BatchSiblings : State->BatchChildren).Contains(Name);
                            return false;
                        })), NAME_None, EUserInterfaceActionType::ToggleButton);
            }
            return Menu.MakeWidget();
        }

        FReply BatchRestructure(int32 Action)
        {
            UOriginPivotComponent* Pivot = Component.Get();
            FText Reason;
            TArray<FName> Names = (Action == 0 ? BatchSiblings : BatchChildren).Array();
            Names.Sort([](FName A, FName B) { return A.LexicalLess(B); });
            bool bOk = false;
            if (Action == 0)
                bOk = FOriginHierarchyAuthoring::AdoptSiblings(Pivot, Names, Reason);
            else if (Action == 1)
                bOk = FOriginHierarchyAuthoring::PromoteChildren(Pivot, Names, Reason);
            else if (Action == 2)
                bOk = FOriginHierarchyAuthoring::DetachChildrenToRoot(Pivot, Names, Reason);
            else if (Action == 3)
            {
                FName NewVariable;
                bOk = FOriginHierarchyAuthoring::InsertSharedPivotAboveChildren(
                    Pivot, Names, NewVariable, Reason);
            }
            else if (Action == 4)
                bOk = FOriginHierarchyAuthoring::TransferChildrenToPivot(
                    Pivot, SelectedTransferTarget, Names, Reason);
            if (!bOk)
                Origin::Notify(Reason.IsEmpty()
                    ? LOCTEXT("BatchSelectionRequired", "Select at least one eligible authored component.") : Reason);
            else
            {
                BatchSiblings.Reset();
                BatchChildren.Reset();
                RefreshHierarchyChoices();
            }
            return FReply::Handled();
        }

        void RefreshSlots()
        {
            const UOriginPivotComponent* Pivot = Component.Get();
            if (!Pivot) return;
            if (!Pivot->OriginPivotConfiguration.FindSlot(SelectedSlotName))
                SelectedSlotName = Pivot->OriginPivotConfiguration.ActiveSlotName;
            SlotOptions.Reset();
            for (const FOriginPivotSlot& Slot : Pivot->OriginPivotConfiguration.Slots)
                SlotOptions.Add(MakeShared<FName>(Slot.Name));
            if (TSharedPtr<SComboBox<TSharedPtr<FName>>> Combo = SlotCombo.Pin())
                Combo->RefreshOptions();
        }

        bool CanAuthorSlots() const
        {
            return FOriginPivotAuthoring::CanAuthor(Component.Get());
        }

        FOriginPivotDefinition Source() const
        { return FOriginPivotAuthoring::ReadSourceDefinition(Component.Get()); }

        void SourceChange(TFunctionRef<void(FOriginPivotDefinition&)> Edit)
        {
            UOriginPivotComponent* Pivot = Component.Get();
            if (!CanAuthorSlots() || !Pivot) return;
            FOriginPivotDefinition Definition = Source();
            Edit(Definition);
            FText Reason;
            if (!FOriginPivotAuthoring::SaveSourceDefinition(Pivot, Definition, Reason))
                Origin::Notify(Reason);
        }

        FReply RecalculateSource()
        {
            FText Reason;
            if (!FOriginPivotAuthoring::RecalculateSourcePivot(Component.Get(), Reason))
                Origin::Notify(Reason);
            return FReply::Handled();
        }

        bool IsSelectedCaptured() const
        {
            const UOriginPivotComponent* Pivot = Component.Get();
            const FOriginPivotSlot* Slot = Pivot ? Pivot->OriginPivotConfiguration.FindSlot(SelectedSlotName) : nullptr;
            return Slot && Slot->bHasSavedRelativeTransform;
        }

        FReply SlotOperation(int32 Operation)
        {
            UOriginPivotComponent* Pivot = Component.Get();
            if (!CanAuthorSlots()) return FReply::Handled();
            FText Reason;
            bool bOk = false;
            if (Operation == 0)
                bOk = FOriginPivotAuthoring::CreateSlot(Pivot, FName(*ProposedSlotName.TrimStartAndEnd()), Reason);
            else if (Operation == 1)
                bOk = FOriginPivotAuthoring::CaptureSlot(Pivot, SelectedSlotName, Reason);
            else if (Operation == 2)
                bOk = FOriginPivotAuthoring::ActivateSlot(Pivot, SelectedSlotName, Reason);
            else if (Operation == 3)
                bOk = FOriginPivotAuthoring::RemoveSlot(Pivot, SelectedSlotName, Reason);
            else if (Operation == 4)
                bOk = FOriginPivotAuthoring::DuplicateSlot(Pivot, SelectedSlotName,
                    FName(*ProposedSlotName.TrimStartAndEnd()), Reason);
            else if (Operation == 5)
                bOk = FOriginPivotAuthoring::RenameSlot(Pivot, SelectedSlotName,
                    FName(*ProposedSlotName.TrimStartAndEnd()), Reason);
            if (bOk)
            {
                if (Operation == 0 || Operation == 5)
                    SelectedSlotName = FName(*ProposedSlotName.TrimStartAndEnd());
                RefreshSlots();
            }
            else Origin::Notify(Reason);
            return FReply::Handled();
        }

        FReply Snap(EOriginPivotSnapAction Mode)
        {
            if (!CanAuthorSlots()) return FReply::Handled();
            FText Reason;
            if (!FOriginPivotAuthoring::SnapPivot(Component.Get(), Mode, Reason))
                Origin::Notify(Reason);
            TranslationDelta = FVector::ZeroVector;
            RotationDelta = FRotator::ZeroRotator;
            return FReply::Handled();
        }


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

    State->SelectedSlotName = State->Component.IsValid()
        ? State->Component->OriginPivotConfiguration.ActiveSlotName : FName(TEXT("Default"));
    State->RefreshSlots();
    State->RefreshHierarchyChoices();

    IDetailGroup& Slots = Category.AddGroup(TEXT("OriginPivotSlots"),
        LOCTEXT("NamedPivotSlots", "Named Pivot Slots"), false, true);
    TSharedPtr<SComboBox<TSharedPtr<FName>>> SlotSelector;
    Slots.AddWidgetRow()
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SelectedSlot", "Selected Slot"))]
        .ValueContent().MinDesiredWidth(240.0f)
        [SAssignNew(SlotSelector, SComboBox<TSharedPtr<FName>>)
            .OptionsSource(&State->SlotOptions)
            .OnGenerateWidget_Lambda([](TSharedPtr<FName> Name) -> TSharedRef<SWidget>
            {
                return SNew(STextBlock).Text(Name.IsValid() ? FText::FromName(*Name) : FText::GetEmpty());
            })
            .OnSelectionChanged_Lambda([State](TSharedPtr<FName> Name, ESelectInfo::Type)
            {
                if (Name.IsValid()) State->SelectedSlotName = *Name;
            })
            .Content()[SNew(STextBlock).Text_Lambda([State]
            {
                return FText::FromName(State->SelectedSlotName);
            })]];
    State->SlotCombo = SlotSelector;

    Slots.AddWidgetRow().WholeRowContent()
        [SNew(STextBlock).AutoWrapText(true)
            .Text_Lambda([WeakPivot, State]
            {
                const UOriginPivotComponent* Target = WeakPivot.Get();
                if (!Target) return FText::GetEmpty();
                const bool bActive = Target->OriginPivotConfiguration.ActiveSlotName == State->SelectedSlotName;
                const FOriginPivotSlot* Slot = Target->OriginPivotConfiguration.FindSlot(State->SelectedSlotName);
                const bool bCaptured = Slot && Slot->bHasSavedRelativeTransform;
                const bool bAtSavedPose = bCaptured &&
                    Target->GetRelativeTransform().Equals(Slot->SavedRelativeTransform, 1.e-4);
                return FText::Format(
                    LOCTEXT("SlotStatus", "{0} | {1} | {2}"),
                    bActive ? LOCTEXT("ActiveStatus", "Active") : LOCTEXT("InactiveStatus", "Inactive"),
                    bCaptured ? LOCTEXT("CapturedStatus", "Captured") : LOCTEXT("UncapturedStatus", "Not captured"),
                    !bCaptured ? LOCTEXT("NoPose", "No saved pose")
                        : bAtSavedPose ? LOCTEXT("SavedPoseMatches", "At saved pose")
                                       : LOCTEXT("SavedPoseDiffers", "Modified since capture"));
            })];
    Slots.AddWidgetRow().WholeRowContent()
        [SNew(STextBlock).AutoWrapText(true)
            .Text_Lambda([State]
            {
                const UOriginPivotComponent* Target = State->Component.Get();
                const FOriginPivotSlot* Slot = Target
                    ? Target->OriginPivotConfiguration.FindSlot(State->SelectedSlotName) : nullptr;
                if (!Slot || !Slot->bHasSavedRelativeTransform) return FText::GetEmpty();
                return FText::Format(LOCTEXT("SavedPivotPose", "Saved local: {0} cm | {1} degrees"),
                    FText::FromString(Slot->SavedRelativeTransform.GetTranslation().ToCompactString()),
                    FText::FromString(Slot->SavedRelativeTransform.Rotator().ToCompactString()));
            })];
    Slots.AddWidgetRow().WholeRowContent()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 2, 5, 2)
            [SNew(SEditableTextBox)
                .Text(FText::FromString(State->ProposedSlotName))
                .HintText(LOCTEXT("NewNameHint", "New slot name"))
                .OnTextChanged_Lambda([State](const FText& Text) { State->ProposedSlotName = Text.ToString(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 0, 2)
            [SNew(SButton)
                .Text(LOCTEXT("CreateNamed", "Capture New Slot"))
                .ToolTipText(LOCTEXT("CreateNamedTip", "Create a new named slot with the current pivot's parent-relative transform. Does not move the assembly."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .OnClicked_Lambda([State] { return State->SlotOperation(0); })]];
    Slots.AddWidgetRow().WholeRowContent()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
            [SNew(SButton)
                .Text(LOCTEXT("OverwriteSlot", "Capture Current"))
                .ToolTipText(LOCTEXT("OverwriteSlotTip", "Overwrite the selected slot pose with the current pivot pose. You can capture the legacy Default slot here."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .OnClicked_Lambda([State] { return State->SlotOperation(1); })]
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
            [SNew(SButton)
                .Text(LOCTEXT("ActivateSelected", "Activate"))
                .ToolTipText(LOCTEXT("ActivateSelectedTip", "Move the pivot to this saved local-space pose while preserving all children's world transforms."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots() && State->IsSelectedCaptured(); })
                .OnClicked_Lambda([State] { return State->SlotOperation(2); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 0, 2)
            [SNew(SButton)
                .Text(LOCTEXT("DeleteSlot", "Delete"))
                .ToolTipText(LOCTEXT("DeleteSlotTip", "Delete the selected named slot, including the active slot. Deleting the active slot selects Default without moving the pivot. Default cannot be deleted."))
                .IsEnabled_Lambda([State]
                {
                    const UOriginPivotComponent* Target = State->Component.Get();
                    return State->CanAuthorSlots() && Target &&
                        State->SelectedSlotName != FName(TEXT("Default")) &&
                        Target->OriginPivotConfiguration.FindSlot(State->SelectedSlotName) != nullptr;
                })
                .OnClicked_Lambda([State] { return State->SlotOperation(3); })]];

    Slots.AddWidgetRow().WholeRowContent()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
            [SNew(SButton)
                .Text(LOCTEXT("DuplicateSlot", "Duplicate as New Name"))
                .ToolTipText(LOCTEXT("DuplicateSlotTip", "Duplicate the selected slot under the name in the text field above, without moving the pivot."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .OnClicked_Lambda([State] { return State->SlotOperation(4); })]
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 0, 2)
            [SNew(SButton)
                .Text(LOCTEXT("RenameSlot", "Rename to New Name"))
                .ToolTipText(LOCTEXT("RenameSlotTip", "Rename the selected slot to the text above. Default cannot be renamed."))
                .IsEnabled_Lambda([State]
                {
                    return State->CanAuthorSlots() && State->SelectedSlotName != FName(TEXT("Default"));
                })
                .OnClicked_Lambda([State] { return State->SlotOperation(5); })]];

    // Source-calculation rules are stored on the Default slot. Editing a rule
    // never changes an explicitly captured pose or moves the assembly; the
    // Recalculate button uses the same transaction service as Snap/Align.
    IDetailGroup& SourceRules = Category.AddGroup(TEXT("OriginPivotSources"),
        LOCTEXT("SourceRules", "Calculated Pivot Source"), false, true);
    SourceRules.AddWidgetRow().WholeRowContent()
        [SNew(STextBlock).AutoWrapText(true)
            .Text(LOCTEXT("SourceHelp", "Choose a source rule, then Recalculate Pivot. Level instances use world coordinates; authored Blueprint components use the pivot parent's local frame. Existing named poses are never overwritten."))];
    SourceRules.AddWidgetRow()
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceMode", "Pivot Mode"))]
        .ValueContent().MinDesiredWidth(240)
        [SNew(SComboButton).IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
            .ButtonContent()[SNew(STextBlock).Text_Lambda([State] {
                return FText::FromString(SourceModeLabel(State->Source().PositionMode));
            })]
            .OnGetMenuContent_Lambda([State] {
                FMenuBuilder Menu(true, nullptr);
                for (int32 Index = 0; Index < 9; ++Index)
                {
                    const EOriginPivotMode Mode = static_cast<EOriginPivotMode>(Index);
                    Menu.AddMenuEntry(FText::FromString(SourceModeLabel(Mode)), FText::GetEmpty(), FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([State, Mode] {
                            State->SourceChange([Mode](FOriginPivotDefinition& Definition) {
                                Definition.PositionMode = Mode;
                            });
                        })));
                }
                return Menu.MakeWidget();
            })];
    auto ShowSourceMode = [State](EOriginPivotMode Mode) {
        return TAttribute<EVisibility>::CreateLambda([State, Mode] {
            return State->Source().PositionMode == Mode ? EVisibility::Visible : EVisibility::Collapsed;
        });
    };
    auto ShowChildSource = TAttribute<EVisibility>::CreateLambda([State] {
        const auto Mode = State->Source().PositionMode;
        return Mode == EOriginPivotMode::ChildActor || Mode == EOriginPivotMode::ComponentSocket
            ? EVisibility::Visible : EVisibility::Collapsed;
    });
    SourceRules.AddWidgetRow().Visibility(ShowChildSource)
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceChildComponent", "Direct Child"))]
        .ValueContent().MinDesiredWidth(240)
        [SNew(SComboButton).IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
            .ButtonContent()[SNew(STextBlock).Text_Lambda([State] {
                const FName Name = State->Source().ComponentName;
                return Name.IsNone() ? LOCTEXT("ChooseSourceChild", "Choose child component") : FText::FromName(Name);
            })]
            .OnGetMenuContent_Lambda([State] {
                FMenuBuilder Menu(true, nullptr);
                TArray<FName> Names;
                FOriginPivotAuthoring::ListDirectSourceComponents(State->Component.Get(), Names);
                for (const FName Name : Names)
                    Menu.AddMenuEntry(FText::FromName(Name), FText::GetEmpty(), FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([State, Name] {
                            State->SourceChange([Name](FOriginPivotDefinition& Definition) {
                                Definition.ComponentName = Name;
                                Definition.SocketName = NAME_None;
                                Definition.ChildActor.Reset();
                            });
                        })));
                return Menu.MakeWidget();
            })];
    SourceRules.AddWidgetRow().Visibility(ShowSourceMode(EOriginPivotMode::ComponentSocket))
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceSocket", "Socket"))]
        .ValueContent().MinDesiredWidth(240)
        [SNew(SComboButton).IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
            .ButtonContent()[SNew(STextBlock).Text_Lambda([State] {
                const FName Socket = State->Source().SocketName;
                return Socket.IsNone() ? LOCTEXT("ComponentOrigin", "Component Origin") : FText::FromName(Socket);
            })]
            .OnGetMenuContent_Lambda([State] {
                FMenuBuilder Menu(true, nullptr);
                TArray<FName> Sockets;
                FOriginPivotAuthoring::ListSourceSockets(State->Component.Get(), State->Source().ComponentName, Sockets);
                for (const FName Socket : Sockets)
                    Menu.AddMenuEntry(Socket.IsNone() ? LOCTEXT("SourceComponentOrigin", "Component Origin") : FText::FromName(Socket),
                        FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([State, Socket] {
                            State->SourceChange([Socket](FOriginPivotDefinition& Definition) {
                                Definition.SocketName = Socket;
                            });
                        })));
                return Menu.MakeWidget();
            })];
    SourceRules.AddWidgetRow().Visibility(ShowSourceMode(EOriginPivotMode::ReferenceActor))
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("ReferenceSourceActor", "Reference Actor"))]
        .ValueContent().MinDesiredWidth(240)
        [SNew(SObjectPropertyEntryBox).AllowedClass(AActor::StaticClass())
            .AllowClear(true).DisplayThumbnail(false)
            .ToolTipText(LOCTEXT("ReferenceSourceTip", "Loaded world Actor outside this assembly. Unavailable on Blueprint component templates."))
            .IsEnabled_Lambda([State] { return State->CanAuthorSlots() && !State->Component->IsTemplate(); })
            .ObjectPath_Lambda([State] { return State->Source().ReferenceActor.ToSoftObjectPath().ToString(); })
            .OnObjectChanged_Lambda([State](const FAssetData& Data) {
                AActor* Actor = Cast<AActor>(Data.GetAsset());
                State->SourceChange([Actor](FOriginPivotDefinition& Definition) {
                    Definition.ReferenceActor = Actor;
                });
            })];

    auto Coordinates = [State](bool bOffset) -> TSharedRef<SWidget> {
        auto Row = SNew(SHorizontalBox);
        for (int32 Axis = 0; Axis < 3; ++Axis)
            Row->AddSlot().FillWidth(1).Padding(2, 0)
            [SNew(SNumericEntryBox<double>).AllowSpin(false)
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .Value_Lambda([State, Axis, bOffset]() -> TOptional<double> {
                    const FOriginPivotDefinition Definition = State->Source();
                    return (bOffset ? Definition.Offset : Definition.CustomPosition)[Axis];
                })
                .OnValueCommitted_Lambda([State, Axis, bOffset](double Value, ETextCommit::Type Commit) {
                    if (Commit == ETextCommit::OnCleared) return;
                    State->SourceChange([Axis, bOffset, Value](FOriginPivotDefinition& Definition) {
                        (bOffset ? Definition.Offset : Definition.CustomPosition)[Axis] = Value;
                    });
                })];
        return Row;
    };
    SourceRules.AddWidgetRow().Visibility(ShowSourceMode(EOriginPivotMode::Custom))
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceCustom", "Custom X / Y / Z"))]
        .ValueContent().MinDesiredWidth(240)[Coordinates(false)];
    auto Axes = SNew(SHorizontalBox);
    for (int32 Axis = 0; Axis < 3; ++Axis)
        Axes->AddSlot().FillWidth(1).Padding(2, 0)
        [SNew(SComboButton).IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
            .ButtonContent()[SNew(STextBlock).Text_Lambda([State, Axis] {
                const int32 Value = State->Source().BoundsAxes[Axis];
                return FText::FromString(Value == 0 ? TEXT("Min") : Value == 2 ? TEXT("Max") : TEXT("Center"));
            })]
            .OnGetMenuContent_Lambda([State, Axis] {
                FMenuBuilder Menu(true, nullptr);
                for (int32 Value = 0; Value < 3; ++Value)
                    Menu.AddMenuEntry(FText::FromString(Value == 0 ? TEXT("Min") : Value == 2 ? TEXT("Max") : TEXT("Center")),
                        FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([State, Axis, Value] {
                            State->SourceChange([Axis, Value](FOriginPivotDefinition& Definition) {
                                Definition.BoundsAxes[Axis] = Value;
                            });
                        })));
                return Menu.MakeWidget();
            })];
    SourceRules.AddWidgetRow().Visibility(ShowSourceMode(EOriginPivotMode::BoundsPoint))
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceAxes", "Bounds X / Y / Z"))]
        .ValueContent().MinDesiredWidth(240)[Axes];
    SourceRules.AddWidgetRow()
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceOffsetSpace", "Offset Space"))]
        .ValueContent().MinDesiredWidth(240)
        [SNew(SComboButton).IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
            .ButtonContent()[SNew(STextBlock).Text_Lambda([State] {
                return State->Source().bLocalOffset ? LOCTEXT("LocalSourceSpace", "Pivot Local")
                    : LOCTEXT("WorldSourceSpace", "Calculation Frame");
            })]
            .OnGetMenuContent_Lambda([State] {
                FMenuBuilder Menu(true, nullptr);
                for (int32 Value = 0; Value < 2; ++Value)
                    Menu.AddMenuEntry(Value ? LOCTEXT("OffsetLocal", "Pivot Local") : LOCTEXT("OffsetFrame", "Calculation Frame"),
                        FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([State, Value] {
                            State->SourceChange([Value](FOriginPivotDefinition& Definition) {
                                Definition.bLocalOffset = Value != 0;
                            });
                        })));
                return Menu.MakeWidget();
            })];
    SourceRules.AddWidgetRow()
        .NameContent()[SNew(STextBlock).Text(LOCTEXT("SourceOffset", "Offset X / Y / Z"))]
        .ValueContent().MinDesiredWidth(240)[Coordinates(true)];
    SourceRules.AddWidgetRow().WholeRowContent()
        [SNew(SButton).Text(LOCTEXT("CalculateSourceButton", "Recalculate Pivot"))
            .ToolTipText(LOCTEXT("CalculateSourceTip", "Apply the current source to the pivot without moving the assembly or overwriting captured named poses. Invalid or missing sources are rejected."))
            .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
            .OnClicked_Lambda([State] { return State->RecalculateSource(); })];

    IDetailGroup& Snap = Category.AddGroup(TEXT("OriginPivotSnap"),
        LOCTEXT("SnapGroup", "Snap / Align Pivot"), false, true);
    Snap.AddWidgetRow().WholeRowContent()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
            [SNew(SButton)
                .Text(LOCTEXT("ToChildren", "Avg Child Origins"))
                .ToolTipText(LOCTEXT("ToChildrenTip", "Move pivot location to the average world positions of direct child component origins (parent-local in Blueprints)."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .OnClicked_Lambda([State] { return State->Snap(EOriginPivotSnapAction::AverageDirectChildren); })]
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
            [SNew(SButton)
                .Text(LOCTEXT("ToParent", "To Parent"))
                .ToolTipText(LOCTEXT("ToParentTip", "Align pivot location to its parent origin. Preserves all child transforms."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .OnClicked_Lambda([State] { return State->Snap(EOriginPivotSnapAction::ParentOrigin); })]
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 0, 2)
            [SNew(SButton)
                .Text(LOCTEXT("MatchParent", "Match Parent Rotation"))
                .ToolTipText(LOCTEXT("MatchParentTip", "Align pivot orientation to parent while retaining pivot position and child world transforms."))
                .IsEnabled_Lambda([State] { return State->CanAuthorSlots(); })
                .OnClicked_Lambda([State] { return State->Snap(EOriginPivotSnapAction::ParentOrientation); })]];
    Snap.AddWidgetRow().WholeRowContent()
        [SNew(STextBlock)
            .Text(LOCTEXT("SnapNote", "These actions move the pivot, not the assembly. Capture Current to store the new pose in a named slot. Blueprint hierarchy operations are in the section below."))
            .AutoWrapText(true)];

    // Structural authoring is intentionally Blueprint-only in this first gate.
    // The controls enumerate authored SCS variables, not preview Actor components.
    if (FOriginHierarchyAuthoring::CanEdit(State->Component.Get()))
    {
        IDetailGroup& Hierarchy = Category.AddGroup(TEXT("OriginHierarchyAuthoring"),
            LOCTEXT("HierarchyGroup", "Blueprint Hierarchy Authoring"), false, true);
        Hierarchy.AddWidgetRow().WholeRowContent()
            [SNew(STextBlock)
                .Text(LOCTEXT("HierarchyHelp", "Choose an authored component, then change its SCS parent while preserving its placement. Each operation is one Undo step. Socket attachments and inherited components are excluded."))
                .AutoWrapText(true)];
        TSharedPtr<SComboBox<TSharedPtr<FName>>> SiblingSelector;
        Hierarchy.AddWidgetRow()
            .NameContent()[SNew(STextBlock).Text(LOCTEXT("SiblingLabel", "Sibling Component"))]
            .ValueContent().MinDesiredWidth(230.0f)
            [SAssignNew(SiblingSelector, SComboBox<TSharedPtr<FName>>)
                .OptionsSource(&State->SiblingOptions)
                .OnGenerateWidget_Lambda([](TSharedPtr<FName> Name) -> TSharedRef<SWidget>
                {
                    return SNew(STextBlock).Text(Name.IsValid() ? FText::FromName(*Name) : FText::GetEmpty());
                })
                .OnSelectionChanged_Lambda([State](TSharedPtr<FName> Name, ESelectInfo::Type)
                {
                    if (Name.IsValid()) State->SelectedSibling = *Name;
                })
                .Content()[SNew(STextBlock).Text_Lambda([State]
                {
                    return State->SelectedSibling.IsNone() ? LOCTEXT("NoSibling", "No eligible sibling") : FText::FromName(State->SelectedSibling);
                })]];
        State->SiblingCombo = SiblingSelector;
        Hierarchy.AddWidgetRow().WholeRowContent()
            [SNew(SButton)
                .Text(LOCTEXT("AdoptSibling", "Reparent Sibling To This Pivot"))
                .ToolTipText(LOCTEXT("AdoptSiblingTip", "Move the selected component from this pivot's SCS parent to this pivot. Preserve the component and its descendants in the same parent frame."))
                .IsEnabled_Lambda([State] { return FOriginHierarchyAuthoring::CanEdit(State->Component.Get()) && !State->SelectedSibling.IsNone(); })
                .OnClicked_Lambda([State] { return State->Restructure(0); })];

        TSharedPtr<SComboBox<TSharedPtr<FName>>> ChildSelector;
        Hierarchy.AddWidgetRow()
            .NameContent()[SNew(STextBlock).Text(LOCTEXT("ChildLabel", "Direct Child"))]
            .ValueContent().MinDesiredWidth(230.0f)
            [SAssignNew(ChildSelector, SComboBox<TSharedPtr<FName>>)
                .OptionsSource(&State->ChildOptions)
                .OnGenerateWidget_Lambda([](TSharedPtr<FName> Name) -> TSharedRef<SWidget>
                {
                    return SNew(STextBlock).Text(Name.IsValid() ? FText::FromName(*Name) : FText::GetEmpty());
                })
                .OnSelectionChanged_Lambda([State](TSharedPtr<FName> Name, ESelectInfo::Type)
                {
                    if (Name.IsValid()) State->SelectedChild = *Name;
                })
                .Content()[SNew(STextBlock).Text_Lambda([State]
                {
                    return State->SelectedChild.IsNone() ? LOCTEXT("NoChild", "No eligible child") : FText::FromName(State->SelectedChild);
                })]];
        State->ChildCombo = ChildSelector;
        Hierarchy.AddWidgetRow().WholeRowContent()
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
                [SNew(SButton)
                    .Text(LOCTEXT("PromoteChild", "Promote To Parent"))
                    .ToolTipText(LOCTEXT("PromoteTip", "Reparent the selected direct child to this pivot's parent, without deleting the pivot."))
                    .IsEnabled_Lambda([State] { return FOriginHierarchyAuthoring::CanEdit(State->Component.Get()) && !State->SelectedChild.IsNone(); })
                    .OnClicked_Lambda([State] { return State->Restructure(1); })]
                + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 0, 2)
                [SNew(SButton)
                    .Text(LOCTEXT("InsertPivot", "Insert Pivot Above"))
                    .ToolTipText(LOCTEXT("InsertPivotTip", "Create a new Origin Pivot directly between the selected child and this pivot. Place the new pivot at the child's origin without moving the branch."))
                    .IsEnabled_Lambda([State] { return FOriginHierarchyAuthoring::CanEdit(State->Component.Get()) && !State->SelectedChild.IsNone(); })
                    .OnClicked_Lambda([State] { return State->Restructure(2); })]];
    }

    if (FOriginHierarchyAuthoring::CanEdit(State->Component.Get()))
    {
        TSharedPtr<SComboBox<TSharedPtr<FName>>> TransferSelector;
        IDetailGroup& Batch = Category.AddGroup(TEXT("OriginHierarchyBatch"),
            LOCTEXT("BatchAuthoringGroup", "Multi-Component Hierarchy Authoring"), false, true);
        Batch.AddWidgetRow().WholeRowContent()
            [SNew(STextBlock)
                .Text(LOCTEXT("BatchHelp", "Choose multiple authored SCS components. Every selected component must be valid; each operation applies as one Undo step or changes nothing. Detach moves components to the authored Blueprint root, not out of the Actor."))
                .AutoWrapText(true)];
        Batch.AddWidgetRow()
            .NameContent()[SNew(STextBlock).Text(LOCTEXT("BatchSiblingLabel", "Siblings To Reparent"))]
            .ValueContent().MinDesiredWidth(230.0f)
            [SNew(SComboButton)
                .OnGetMenuContent_Lambda([State] { return State->MakeBatchSelectionMenu(true); })
                .ButtonContent()[SNew(STextBlock).Text_Lambda([State]
                {
                    return FText::Format(LOCTEXT("BatchSiblingsCount", "{0} selected"),
                        FText::AsNumber(State->BatchSiblings.Num()));
                })]];
        Batch.AddWidgetRow().WholeRowContent()
            [SNew(SButton)
                .Text(LOCTEXT("BatchReparent", "Reparent Selected Siblings To This Pivot"))
                .ToolTipText(LOCTEXT("BatchReparentTip", "Move all selected direct siblings beneath this Pivot, preserving every branch, in one Undo step."))
                .IsEnabled_Lambda([State] { return !State->BatchSiblings.IsEmpty() &&
                    FOriginHierarchyAuthoring::CanEdit(State->Component.Get()); })
                .OnClicked_Lambda([State] { return State->BatchRestructure(0); })];
        Batch.AddWidgetRow()
            .NameContent()[SNew(STextBlock).Text(LOCTEXT("BatchChildLabel", "Children To Restructure"))]
            .ValueContent().MinDesiredWidth(230.0f)
            [SNew(SComboButton)
                .OnGetMenuContent_Lambda([State] { return State->MakeBatchSelectionMenu(false); })
                .ButtonContent()[SNew(STextBlock).Text_Lambda([State]
                {
                    return FText::Format(LOCTEXT("BatchChildrenCount", "{0} selected"),
                        FText::AsNumber(State->BatchChildren.Num()));
                })]];
        Batch.AddWidgetRow().WholeRowContent()
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 4, 2)
                [SNew(SButton)
                    .Text(LOCTEXT("BatchPromote", "Promote Selected"))
                    .ToolTipText(LOCTEXT("BatchPromoteTip", "Move all selected direct children to this Pivot's parent, retaining their world placement."))
                    .IsEnabled_Lambda([State] { return !State->BatchChildren.IsEmpty() &&
                        FOriginHierarchyAuthoring::CanEdit(State->Component.Get()); })
                    .OnClicked_Lambda([State] { return State->BatchRestructure(1); })]
                + SHorizontalBox::Slot().FillWidth(1).Padding(0, 2, 0, 2)
                [SNew(SButton)
                    .Text(LOCTEXT("BatchInsertShared", "Insert Shared Pivot"))
                    .ToolTipText(LOCTEXT("BatchInsertSharedTip", "Insert one new Pivot above all selected direct children, at their average authored origin."))
                    .IsEnabled_Lambda([State] { return !State->BatchChildren.IsEmpty() &&
                        FOriginHierarchyAuthoring::CanEdit(State->Component.Get()); })
                    .OnClicked_Lambda([State] { return State->BatchRestructure(3); })]];
        Batch.AddWidgetRow()
            .NameContent()[SNew(STextBlock).Text(LOCTEXT("TransferTargetLabel", "Transfer Destination Pivot"))]
            .ValueContent().MinDesiredWidth(230.0f)
            [SAssignNew(TransferSelector, SComboBox<TSharedPtr<FName>>)
                .OptionsSource(&State->TransferTargetOptions)
                .OnGenerateWidget_Lambda([](TSharedPtr<FName> Name) -> TSharedRef<SWidget>
                {
                    return SNew(STextBlock).Text(Name.IsValid() ? FText::FromName(*Name) : FText::GetEmpty());
                })
                .OnSelectionChanged_Lambda([State](TSharedPtr<FName> Name, ESelectInfo::Type)
                {
                    if (Name.IsValid()) State->SelectedTransferTarget = *Name;
                })
                .Content()[SNew(STextBlock).Text_Lambda([State]
                {
                    return State->SelectedTransferTarget.IsNone()
                        ? LOCTEXT("NoTransferTarget", "No compatible Pivot")
                        : FText::FromName(State->SelectedTransferTarget);
                })]];
        State->TransferTargetCombo = TransferSelector;
        Batch.AddWidgetRow().WholeRowContent()
            [SNew(SButton)
                .Text(LOCTEXT("BatchTransfer", "Transfer Selected Children To Pivot"))
                .ToolTipText(LOCTEXT("BatchTransferTip", "Move selected direct children to another authored Origin Pivot in this Blueprint. Keeps complete branches in place, rejects cycles, sockets and unsafe inherited transforms, and performs one Undo transaction."))
                .IsEnabled_Lambda([State] { return !State->BatchChildren.IsEmpty() &&
                    !State->SelectedTransferTarget.IsNone() &&
                    FOriginHierarchyAuthoring::CanEdit(State->Component.Get()); })
                .OnClicked_Lambda([State] { return State->BatchRestructure(4); })];
        Batch.AddWidgetRow().WholeRowContent()
            [SNew(SButton)
                .Text(LOCTEXT("BatchDetachToRoot", "Detach Selected Children To Blueprint Root"))
                .ToolTipText(LOCTEXT("BatchDetachTip", "Move selected direct children under the authored Blueprint scene root, without moving them in the Actor. Rejects inherited, socket or absolute-transform paths."))
                .IsEnabled_Lambda([State] { return !State->BatchChildren.IsEmpty() &&
                    FOriginHierarchyAuthoring::CanEdit(State->Component.Get()); })
                .OnClicked_Lambda([State] { return State->BatchRestructure(2); })];
    }

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
