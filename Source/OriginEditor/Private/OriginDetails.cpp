#include "OriginDetails.h"
#include "OriginOperations.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SCheckBox.h"
#include "Components/SceneComponent.h"
#include "PropertyCustomizationHelpers.h"
#include "AssetRegistry/AssetData.h"
#include "HAL/PlatformTime.h"
#include "IDetailGroup.h"
#include "Textures/SlateIcon.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateStyle.h"

namespace
{
    const TCHAR* ModeLabel(EOriginPivotMode Mode)
    {
        switch (Mode)
        {
        case EOriginPivotMode::BoundsCenter: return TEXT("Bounds Center");
        case EOriginPivotMode::AverageChildPivots: return TEXT("Average Child Pivots");
        case EOriginPivotMode::BottomCenter: return TEXT("Bottom Center");
        case EOriginPivotMode::ChildActor: return TEXT("Child Actor");
        case EOriginPivotMode::WorldOrigin: return TEXT("World Origin");
        case EOriginPivotMode::BoundsPoint: return TEXT("Bounds Point");
        case EOriginPivotMode::ReferenceActor: return TEXT("Reference Actor");
        case EOriginPivotMode::ComponentSocket: return TEXT("Component / Socket");
        default: return TEXT("Custom");
        }
    }
    // Shared by this panel's widgets only. Poll source/lock state at most four
    // times a second, rather than traversing the assembly for every attribute.
    struct FPanelState
    {
        TWeakObjectPtr<AOriginAnchor> Anchor;
        double NextCheck=0;
        bool bCanEdit=false, bCanRecalculate=false;
        FText EditReason, RecalculateReason;
        void Update()
        {
            const double Now=FPlatformTime::Seconds();
            if(Now<NextCheck) return;
            NextCheck=Now+0.25;
            bCanEdit=Origin::CanOperate(Anchor.Get(),EditReason);
            bCanRecalculate=bCanEdit && Origin::CanRecalculate(Anchor.Get(),RecalculateReason);
            if(!bCanEdit) RecalculateReason=EditReason;
        }
        void Change(TFunctionRef<void(Origin::FPivotSettings&)> Edit)
        {
            if(auto* A=Anchor.Get())
            {
                auto Settings=Origin::ReadSettings(A); Edit(Settings);
                Origin::ApplySettings(A,Settings);
            }
            NextCheck=0;
        }
    };
    bool IsChildMode(EOriginPivotMode Mode)
    { return Mode==EOriginPivotMode::ChildActor || Mode==EOriginPivotMode::ComponentSocket; }
    USceneComponent* SourceComponent(AOriginAnchor* A)
    {
        if(!A || !A->PivotChild.IsValid()) return nullptr;
        TArray<USceneComponent*> Components; A->PivotChild->GetComponents(Components);
        for(auto* C:Components) if(IsValid(C) && C->GetFName()==A->PivotComponentName) return C;
        return nullptr;
    }
    TSharedRef<SWidget> Choice(const TSharedRef<FPanelState>& State, const TCHAR* Tooltip,
        TFunction<FText()> Label, TFunction<TSharedRef<SWidget>()> Menu)
    {
        return SNew(SComboButton).ToolTipText(FText::FromString(Tooltip))
            .IsEnabled_Lambda([State] { State->Update(); return State->bCanEdit; })
            .OnGetMenuContent_Lambda([Menu] { return Menu(); })
            .ButtonContent()[SNew(STextBlock).Text_Lambda([Label] { return Label(); })];
    }
    void Entry(FMenuBuilder& Menu, const FString& Label, TFunction<void()> Action)
    {
        Menu.AddMenuEntry(FText::FromString(Label),FText::GetEmpty(),FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([Action] { Action(); })));
    }
    TSharedRef<SWidget> Coordinates(const TSharedRef<FPanelState>& State, bool Offset)
    {
        auto Box=SNew(SHorizontalBox);
        for(int32 Axis=0;Axis<3;++Axis)
            Box->AddSlot().FillWidth(1).Padding(2,0)
            [SNew(SNumericEntryBox<double>).AllowSpin(false)
                .ToolTipText(FText::FromString(FString(Offset ? TEXT("Offset ") : TEXT("World ")) +
                    (Axis==0 ? TEXT("X (cm)") : Axis==1 ? TEXT("Y (cm)") : TEXT("Z (cm)"))))
                .IsEnabled_Lambda([State] { State->Update(); return State->bCanEdit; })
                .Value_Lambda([State,Axis,Offset]() -> TOptional<double> {
                    auto* A=State->Anchor.Get();
                    return A ? TOptional<double>((Offset ? A->OriginPivotOffset : A->CustomPivot)[Axis]) : TOptional<double>();
                })
                .OnValueCommitted_Lambda([State,Axis,Offset](double Value,ETextCommit::Type Commit) {
                    if(Commit==ETextCommit::OnCleared) return;
                    State->Change([=](Origin::FPivotSettings& S) { (Offset ? S.Offset : S.Custom)[Axis]=Value; });
                })];
        return Box;
    }
}
void FOriginDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
    TArray<TWeakObjectPtr<UObject>> Objects; Builder.GetObjectsBeingCustomized(Objects);
    auto& Category = Builder.EditCategory(TEXT("Origin"), FText::FromString(TEXT("Origin")), ECategoryPriority::Important);
    // Whole-row header replaces the default text, while Unreal retains its
    // native category expander and expansion-state handling.
    Category.HeaderContent(
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4,0,6,0)
        [SNew(SBox).WidthOverride(14).HeightOverride(14)
            [SNew(SImage)
                .Image(FSlateStyleRegistry::FindSlateStyle(TEXT("OriginStyle"))->GetBrush(TEXT("Origin.Anchor")))
                .ColorAndOpacity(FSlateColor::UseForeground())]]
        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
        [SNew(STextBlock).Text(FText::FromString(TEXT("Origin")))
            .Font(FAppStyle::Get().GetFontStyle(TEXT("DetailsView.CategoryFontStyle")))
            .TextStyle(FAppStyle::Get(), TEXT("DetailsView.CategoryTextStyle"))], true);

    Builder.SortCategories([](const TMap<FName, IDetailCategoryBuilder*>& Categories)
    {
        IDetailCategoryBuilder* Transform = Categories.FindRef(TEXT("TransformCommon"));
        if (!Transform) Transform = Categories.FindRef(TEXT("Transform"));
        IDetailCategoryBuilder* OriginCategory = Categories.FindRef(TEXT("Origin"));
        if (!Transform || !OriginCategory) return;

        // Surface is optional. When present, keep its existing sections together
        // after Origin without linking to or changing the Surface plugin.
        TArray<IDetailCategoryBuilder*> Inserted;
        Inserted.Add(OriginCategory);
        for (const FName Name : {FName(TEXT("Surface_Favorites")), FName(TEXT("Surface_Components")), FName(TEXT("Surface_Selection"))})
            if (IDetailCategoryBuilder* Other = Categories.FindRef(Name)) Inserted.AddUnique(Other);
        const int32 TransformOrder = Transform->GetSortOrder();
        const int32 Count = Inserted.Num();
        if (TransformOrder >= MAX_int32 - Count) return;
        for (const auto& Pair : Categories)
        {
            IDetailCategoryBuilder* Other = Pair.Value;
            if (!Other || Other == Transform || Inserted.Contains(Other)) continue;
            const int32 Order = Other->GetSortOrder();
            if (Order > TransformOrder && Order <= MAX_int32 - Count)
                Other->SetSortOrder(Order + Count);
        }
        for (int32 Index = 0; Index < Count; ++Index)
            Inserted[Index]->SetSortOrder(TransformOrder + Index + 1);
    });
    if (Objects.Num() != 1 || !Cast<AOriginAnchor>(Objects[0].Get()))
    {
        Category.AddCustomRow(FText::FromString(TEXT("Origin"))).WholeRowContent()
        [SNew(STextBlock).Text(FText::FromString(TEXT("Select one Origin Anchor to edit its pivot. Use the Outliner menu to convert multiple anchors."))).AutoWrapText(true)];
        return;
    }
    const TWeakObjectPtr<AOriginAnchor> Weak(Cast<AOriginAnchor>(Objects[0].Get()));
    Category.AddCustomRow(FText::FromString(TEXT("Origin Anchor"))).WholeRowContent().VAlign(VAlign_Center)
    [SNew(STextBlock).Text(FText::FromString(TEXT("Pivot changes preserve child world transforms."))).AutoWrapText(true)];
    const auto State=MakeShared<FPanelState>(); State->Anchor=Weak;
    auto VisibleMode=[Weak](EOriginPivotMode Mode) {
        return TAttribute<EVisibility>::CreateLambda([Weak,Mode] {
            return Weak.IsValid() && Weak->PivotMode==Mode ? EVisibility::Visible : EVisibility::Collapsed;
        });
    };
    Category.AddCustomRow(FText::FromString(TEXT("Show Billboard")))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Show Billboard")))]
        .ValueContent()
        [SNew(SCheckBox)
            .ToolTipText(FText::FromString(TEXT("Show this anchor's clickable viewport marker. Editor only; hide it here to reduce clutter.")))
            .IsEnabled_Lambda([State] { State->Update(); return State->bCanEdit; })
            .IsChecked_Lambda([Weak] { return Weak.IsValid() && Weak->bShowEditorBillboard ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([Weak](ECheckBoxState Check) {
                auto* A=Weak.Get(); FText Reason;
                if(!Origin::CanOperate(A,Reason)) { Origin::Notify(Reason); return; }
                const bool Show=Check==ECheckBoxState::Checked;
                if(A->bShowEditorBillboard==Show) return;
                const FScopedTransaction Transaction(NSLOCTEXT("Origin","BillboardVisibility","Origin: Change billboard visibility"));
                A->Modify(); A->bShowEditorBillboard=Show;
                A->RefreshEditorBillboard(); A->MarkPackageDirty();
                GEditor->RedrawLevelEditingViewports();
            })];
    Category.AddCustomRow(FText::FromString(TEXT("Pivot Mode")))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Pivot Mode")))]
        .ValueContent().MinDesiredWidth(240)
        [Choice(State,TEXT("Choose a pivot rule. Valid settings apply immediately; children stay in place."),
            [Weak] { return FText::FromString(Weak.IsValid() ? ModeLabel(Weak->PivotMode) : TEXT("Unavailable")); },
            [State] {
                FMenuBuilder Menu(true,nullptr);
                for(int32 I=0;I<9;++I)
                {
                    const auto Mode=static_cast<EOriginPivotMode>(I);
                    Entry(Menu,ModeLabel(Mode),[State,Mode] {
                        State->Change([State,Mode](Origin::FPivotSettings& S) {
                            S.Mode=Mode;
                            if(IsChildMode(Mode) && !S.Child.IsValid())
                            {
                                auto Children=Origin::DirectChildren(State->Anchor.Get());
                                if(!Children.IsEmpty()) S.Child=Children[0];
                            }
                            if(Mode==EOriginPivotMode::ComponentSocket && S.ComponentName.IsNone() && S.Child.IsValid())
                                if(auto* Root=S.Child->GetRootComponent()) S.ComponentName=Root->GetFName();
                        });
                    });
                }
                return Menu.MakeWidget();
            })];
    Category.AddCustomRow(FText::FromString(TEXT("Child Actor")))
        .Visibility(TAttribute<EVisibility>::CreateLambda([Weak] {
            return Weak.IsValid() && IsChildMode(Weak->PivotMode) ? EVisibility::Visible : EVisibility::Collapsed;
        }))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Child Actor")))]
        .ValueContent().MinDesiredWidth(240)
        [SNew(SObjectPropertyEntryBox).AllowedClass(AActor::StaticClass()).AllowClear(true).DisplayThumbnail(false)
            .ToolTipText(FText::FromString(TEXT("Choose a direct child using the searchable actor picker or viewport eyedropper.")))
            .IsEnabled_Lambda([State] { State->Update(); return State->bCanEdit; })
            .ObjectPath_Lambda([Weak] { return Weak.IsValid() ? Weak->PivotChild.ToSoftObjectPath().ToString() : FString(); })
            .OnShouldFilterActor_Lambda([Weak](const AActor* Candidate) {
                return Weak.IsValid() && Origin::IsEditorActor(Candidate) && Candidate->GetAttachParentActor()==Weak.Get();
            })
            .OnObjectChanged_Lambda([State](const FAssetData& Data) {
                AActor* Child=Cast<AActor>(Data.GetAsset());
                auto* Anchor=State->Anchor.Get();
                if(Child && (!Origin::IsEditorActor(Child) || Child->GetAttachParentActor()!=Anchor)) return;
                State->Change([Child](Origin::FPivotSettings& S) {
                    S.Child=Child; S.ComponentName=NAME_None; S.SocketName=NAME_None;
                    if(Child && Child->GetRootComponent()) S.ComponentName=Child->GetRootComponent()->GetFName();
                });
            })];
    Category.AddCustomRow(FText::FromString(TEXT("Custom Position"))).Visibility(VisibleMode(EOriginPivotMode::Custom))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("World X / Y / Z")))]
        .ValueContent().MinDesiredWidth(280)[Coordinates(State,false)];

    auto BoundsControls=SNew(SHorizontalBox);
    for(int32 Axis=0;Axis<3;++Axis)
    {
        BoundsControls->AddSlot().FillWidth(1).Padding(2,0)
        [Choice(State,TEXT("World-space bounds: choose minimum, center or maximum on this axis."),
            [Weak,Axis] {
                const int32 Value=Weak.IsValid() ? Weak->BoundsAxes[Axis] : 1;
                return FText::FromString(FString(Axis==0 ? TEXT("X: ") : Axis==1 ? TEXT("Y: ") : TEXT("Z: "))+
                    (Value==0 ? TEXT("Min") : Value==2 ? TEXT("Max") : TEXT("Center")));
            },
            [State,Axis] {
                FMenuBuilder Menu(true,nullptr);
                for(int32 Value=0;Value<3;++Value)
                    Entry(Menu,Value==0 ? TEXT("Min") : Value==2 ? TEXT("Max") : TEXT("Center"),
                        [State,Axis,Value] { State->Change([=](Origin::FPivotSettings& S) { S.BoundsAxes[Axis]=Value; }); });
                return Menu.MakeWidget();
            })];
    }
    Category.AddCustomRow(FText::FromString(TEXT("Bounds Point World Axes"))).Visibility(VisibleMode(EOriginPivotMode::BoundsPoint))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("World Bounds X / Y / Z")))]
        .ValueContent().MinDesiredWidth(300)[BoundsControls];
    Category.AddCustomRow(FText::FromString(TEXT("Bounds Preset"))).Visibility(VisibleMode(EOriginPivotMode::BoundsPoint))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Bounds Preset")))]
        .ValueContent()[Choice(State,TEXT("Choose a common bounds point. Axes above show the resulting position."),
            [] { return FText::FromString(TEXT("Choose preset")); }, [State] {
                FMenuBuilder Menu(true,nullptr);
                const TCHAR* Names[]={TEXT("Center"),TEXT("Top Center"),TEXT("Bottom Center"),TEXT("Min X Face"),TEXT("Max X Face"),TEXT("Min Y Face"),TEXT("Max Y Face")};
                const FIntVector Points[]={FIntVector(1,1,1),FIntVector(1,1,2),FIntVector(1,1,0),FIntVector(0,1,1),FIntVector(2,1,1),FIntVector(1,0,1),FIntVector(1,2,1)};
                for(int32 I=0;I<7;++I) { const FIntVector Point=Points[I];
                    Entry(Menu,Names[I],[State,Point] { State->Change([Point](Origin::FPivotSettings& S) { S.BoundsAxes=Point; }); }); }
                return Menu.MakeWidget();
            })];
    Category.AddCustomRow(FText::FromString(TEXT("Reference Actor"))).Visibility(VisibleMode(EOriginPivotMode::ReferenceActor))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Reference Actor")))]
        .ValueContent().MinDesiredWidth(280)
        [SNew(SObjectPropertyEntryBox).AllowedClass(AActor::StaticClass()).AllowClear(true).DisplayThumbnail(false)
            .ToolTipText(FText::FromString(TEXT("Choose a loaded actor outside this assembly using the picker or eyedropper. Its position is sampled when recalculating.")))
            .IsEnabled_Lambda([State] { State->Update(); return State->bCanEdit; })
            .ObjectPath_Lambda([Weak] { return Weak.IsValid() ? Weak->ReferenceActor.ToSoftObjectPath().ToString() : FString(); })
            // This public actor-picker delegate uses true to mean allowed.
            .OnShouldFilterActor_Lambda([Weak](const AActor* Candidate) {
                if(!Weak.IsValid() || !Origin::IsEditorActor(Candidate) || Candidate->GetWorld()!=Weak->GetWorld()) return false;
                for(const AActor* P=Candidate;P;P=P->GetAttachParentActor()) if(P==Weak.Get()) return false;
                return true;
            })
            .OnObjectChanged_Lambda([State](const FAssetData& Data) {
                AActor* Reference=Cast<AActor>(Data.GetAsset());
                State->Change([Reference](Origin::FPivotSettings& S) { S.Reference=Reference; });
            })];
    Category.AddCustomRow(FText::FromString(TEXT("Component"))).Visibility(VisibleMode(EOriginPivotMode::ComponentSocket))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Component")))]
        .ValueContent().MinDesiredWidth(240)
        [Choice(State,TEXT("Choose a scene component on the selected child. A missing component never silently falls back to another."),
            [Weak] { return FText::FromString(Weak.IsValid() && !Weak->PivotComponentName.IsNone() ? Weak->PivotComponentName.ToString() : TEXT("Choose a component")); },
            [State] {
                FMenuBuilder Menu(true,nullptr);
                if(auto* A=State->Anchor.Get(); A && A->PivotChild.IsValid())
                {
                    TArray<USceneComponent*> Components; A->PivotChild->GetComponents(Components);
                    Components.RemoveAll([](const USceneComponent* C) { return !IsValid(C); });
                    Components.Sort([](const USceneComponent& L,const USceneComponent& R) { return L.GetName()<R.GetName(); });
                    for(auto* C:Components) if(IsValid(C) && C->IsRegistered())
                    { const FName Name=C->GetFName();
                        Entry(Menu,C->GetName(),[State,Name] { State->Change([Name](Origin::FPivotSettings& S) { S.ComponentName=Name; S.SocketName=NAME_None; }); }); }
                }
                return Menu.MakeWidget();
            })];
    Category.AddCustomRow(FText::FromString(TEXT("Socket")))
        .Visibility(TAttribute<EVisibility>::CreateLambda([Weak] {
            auto* A=Weak.Get(); auto* C=SourceComponent(A);
            return A && A->PivotMode==EOriginPivotMode::ComponentSocket &&
                (!A->PivotSocketName.IsNone() || (C && !C->GetAllSocketNames().IsEmpty())) ? EVisibility::Visible : EVisibility::Collapsed;
        }))
        .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Socket")))]
        .ValueContent().MinDesiredWidth(240)
        [Choice(State,TEXT("Use Component Origin or a named socket. Socket position is sampled, not followed continuously."),
            [Weak] { return FText::FromString(Weak.IsValid() && !Weak->PivotSocketName.IsNone() ? Weak->PivotSocketName.ToString() : TEXT("Component Origin")); },
            [State] {
                FMenuBuilder Menu(true,nullptr);
                Entry(Menu,TEXT("Component Origin"),[State] { State->Change([](Origin::FPivotSettings& S) { S.SocketName=NAME_None; }); });
                if(auto* C=SourceComponent(State->Anchor.Get())) for(const FName Name:C->GetAllSocketNames())
                    Entry(Menu,Name.ToString(),[State,Name] { State->Change([Name](Origin::FPivotSettings& S) { S.SocketName=Name; }); });
                return Menu.MakeWidget();
            })];

    auto& OffsetGroup=Category.AddGroup(TEXT("OriginPivotOffset"),FText::FromString(TEXT("Pivot Offset")),false,false);
    OffsetGroup.AddWidgetRow().NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Space")))]
        .ValueContent()[Choice(State,TEXT("World uses world axes. Anchor Local uses anchor rotation without scaling the offset distances."),
            [Weak] { return FText::FromString(Weak.IsValid() && Weak->bLocalPivotOffset ? TEXT("Anchor Local") : TEXT("World")); },
            [State] {
                FMenuBuilder Menu(true,nullptr);
                Entry(Menu,TEXT("World"),[State] { State->Change([](Origin::FPivotSettings& S) { S.bLocalOffset=false; }); });
                Entry(Menu,TEXT("Anchor Local"),[State] { State->Change([](Origin::FPivotSettings& S) { S.bLocalOffset=true; }); });
                return Menu.MakeWidget();
            })];
    OffsetGroup.AddWidgetRow().NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Offset X / Y / Z")))]
        .ValueContent().MinDesiredWidth(280)[Coordinates(State,true)];
    OffsetGroup.AddWidgetRow().WholeRowContent()
        [SNew(SButton).Text(FText::FromString(TEXT("Reset Offset")))
            .ToolTipText(FText::FromString(TEXT("Set all offset coordinates to zero, keeping the chosen space.")))
            .IsEnabled_Lambda([State,Weak] { State->Update(); return State->bCanEdit && Weak.IsValid() && !Weak->OriginPivotOffset.IsZero(); })
            .OnClicked_Lambda([State] { State->Change([](Origin::FPivotSettings& S) { S.Offset=FVector::ZeroVector; }); return FReply::Handled(); })];
    Category.AddCustomRow(FText::FromString(TEXT("Pivot Status"))).WholeRowContent().VAlign(VAlign_Center)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([State] { State->Update(); return State->RecalculateReason; })];
    Category.AddCustomRow(FText::FromString(TEXT("Recalculate Release Convert"))).WholeRowContent()
    [SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0,4,4,4)
        // Tooltip on the enabled wrapper stays available over a disabled button.
        [SNew(SBox).ToolTipText_Lambda([State] { State->Update(); return State->RecalculateReason; })
            [SNew(SButton).Text(FText::FromString(TEXT("Recalculate Pivot")))
                .IsEnabled_Lambda([State] { State->Update(); return State->bCanRecalculate; })
                .OnClicked_Lambda([State] {
                    if(auto* A=State->Anchor.Get()) Origin::Recalculate(A);
                    State->NextCheck=0; return FReply::Handled();
                })]]
        + SHorizontalBox::Slot().AutoWidth().Padding(0,4,4,4)
        [SNew(SButton).Text(FText::FromString(TEXT("Release Children")))
            .ToolTipText(FText::FromString(TEXT("Detach direct children to the world, preserving their transforms, then remove this anchor. Supports Undo.")))
            .OnClicked_Lambda([Weak] { if(auto* A=Weak.Get()) Origin::RemoveAnchors({A},false); return FReply::Handled(); })]
        + SHorizontalBox::Slot().AutoWidth().Padding(0,4,0,4)
        [SNew(SButton).Text(FText::FromString(TEXT("Convert to Empty Actor")))
            .ToolTipText(FText::FromString(TEXT("Replace this anchor with a native empty Actor, keeping the assembly and removing Origin-specific settings.")))
            .OnClicked_Lambda([Weak] { if(auto* A=Weak.Get()) Origin::RemoveAnchors({A},true); return FReply::Handled(); })]];
}
