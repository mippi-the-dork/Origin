// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "OriginOperations.h"
#include "OriginDetails.h"
#include "OriginHierarchy.h"
#include "OriginHierarchyFeedback.h"
#include "SceneOutlinerModule.h"
#include "ActorBrowsingMode.h"
#include "SSceneOutliner.h"
#include "SceneOutlinerPublicTypes.h"
#include "PropertyEditorModule.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ToolMenus.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateImageBrush.h"
#include "Textures/SlateIcon.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"

class FOriginCommands : public TCommands<FOriginCommands>
{
public:
    FOriginCommands() : TCommands<FOriginCommands>(TEXT("Origin"), NSLOCTEXT("Origin","Commands","Origin"), NAME_None, FAppStyle::GetAppStyleSetName()) {}
    TSharedPtr<FUICommandInfo> Create, Convert;
    virtual void RegisterCommands() override
    {
#define LOCTEXT_NAMESPACE "Origin"
        // Deliberately unbound: users can assign a chord in Editor Preferences
        // without replacing Unreal's grouping shortcut or another plugin's key.
        UI_COMMAND(Create,"Create Origin Anchor","Create an anchor above the selected actors, preserving their world transforms. With no actors selected, create an empty anchor at world origin.",EUserInterfaceActionType::Button,FInputChord());
        UI_COMMAND(Convert,"Convert Selected Origin Anchors","Replace selected anchors with native empty Actors while preserving the assembly.",EUserInterfaceActionType::Button,FInputChord());
#undef LOCTEXT_NAMESPACE
    }
};

/** Standard actor browsing behavior, with one extra toolbar widget. */
class FOriginBrowsingMode final : public FActorBrowsingMode
{
    TSharedPtr<FSlateStyleSet> Style;
public:
    FOriginBrowsingMode(SSceneOutliner* View, TWeakObjectPtr<UWorld> World, TSharedPtr<FSlateStyleSet> InStyle)
        : FActorBrowsingMode(View,World), Style(MoveTemp(InStyle)) {}
    virtual bool CanCustomizeToolbar() const override { return true; }
    virtual void CustomAddToToolbar(TSharedPtr<SHorizontalBox> Toolbar) override
    {
        const auto KeepStyle = Style;
        Toolbar->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4,0,0,0)
        [SNew(SButton).ButtonStyle(FAppStyle::Get(),TEXT("SimpleButton"))
            .ToolTipText(FText::FromString(TEXT("Create Origin Anchor\nParent selected actors beneath an anchor without changing their world transforms. With no selection, create an empty anchor at world origin.")))
            .IsEnabled_Lambda([] { return GEditor && !GEditor->PlayWorld; })
            .OnClicked_Lambda([] { Origin::CreateAnchor(); return FReply::Handled(); })
            [SNew(SBox).WidthOverride(18).HeightOverride(18)
                [SNew(SImage).Image(KeepStyle->GetBrush(TEXT("Origin.AnchorAdd"))).ColorAndOpacity(FSlateColor::UseForeground())]]];
        // SupportsCreateNewFolder remains inherited and true. SSceneOutliner
        // appends the native folder button separately after custom controls.
    }
};

class FOriginEditorModule final : public IModuleInterface
{
    TSharedPtr<FSlateStyleSet> Style;
    TSharedPtr<FUICommandList> Commands;
    TWeakPtr<FUICommandList> LevelCommands;
    FDelegateHandle Columns;
    TSharedPtr<IInputProcessor> HierarchyFeedbackProcessor;
    bool bRegistered = false;
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        for (const FName Name : {FName(TEXT("LevelEditor.ActorContextMenu")), FName(TEXT("LevelEditor.LevelEditorSceneOutliner.ContextMenu"))})
        {
            UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(Name);
            auto& Section = Menu->FindOrAddSection(TEXT("Origin"));
            Section.Label = FText::FromString(TEXT("Origin"));
            Section.AddMenuEntryWithCommandList(FOriginCommands::Get().Create,Commands);
            Section.AddMenuEntryWithCommandList(FOriginCommands::Get().Convert,Commands);
        }
    }
public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Origin"));
        if (!Plugin) return;
        Style = MakeShared<FSlateStyleSet>(TEXT("OriginStyle"));
        const FString Resources = FPaths::Combine(Plugin->GetBaseDir(),TEXT("Resources"));
        auto AddBrush = [this,&Resources](const TCHAR* Key,const TCHAR* File,FVector2D Size) {
            Style->Set(Key,new FSlateVectorImageBrush(FPaths::Combine(Resources,File),Size));
        };
        AddBrush(TEXT("Origin.AnchorAdd"),TEXT("origin-anchor-add.svg"),FVector2D(18,18));
        AddBrush(TEXT("Origin.Anchor"),TEXT("origin-anchor.svg"),FVector2D(16,16));
        AddBrush(TEXT("ClassIcon.OriginAnchor"),TEXT("origin-anchor.svg"),FVector2D(16,16));
        AddBrush(TEXT("ClassThumbnail.OriginAnchor"),TEXT("origin-anchor.svg"),FVector2D(64,64));
        AddBrush(TEXT("Origin.Children"),TEXT("parent-select-top.svg"),FVector2D(16,8));
        AddBrush(TEXT("Origin.Descendants"),TEXT("parent-select-bottom.svg"),FVector2D(16,8));
        AddBrush(TEXT("Origin.Parent"),TEXT("child-select-left.svg"),FVector2D(8,16));
        AddBrush(TEXT("Origin.Siblings"),TEXT("child-select-right.svg"),FVector2D(8,16));
        FSlateStyleRegistry::RegisterSlateStyle(*Style);
        FOriginCommands::Register();
        Commands = MakeShared<FUICommandList>();
        Commands->MapAction(FOriginCommands::Get().Create,FExecuteAction::CreateStatic(&Origin::CreateAnchor),
            FCanExecuteAction::CreateLambda([] { return GEditor && !GEditor->PlayWorld; }));
        Commands->MapAction(FOriginCommands::Get().Convert,FExecuteAction::CreateStatic(&Origin::ConvertSelected),
            FCanExecuteAction::CreateLambda([] {
                if (!GEditor || GEditor->PlayWorld) return false;
                for (AActor* A : Origin::SelectedActors()) if (Cast<AOriginAnchor>(A)) return true;
                return false;
            }));
        auto& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        LevelCommands = LevelEditor.GetGlobalLevelEditorActions();
        LevelEditor.GetGlobalLevelEditorActions()->Append(Commands.ToSharedRef());
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,&FOriginEditorModule::RegisterMenus));
        auto& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
        Properties.RegisterCustomClassLayout(AOriginAnchor::StaticClass()->GetFName(),FOnGetDetailCustomizationInstance::CreateLambda([] { return MakeShared<FOriginDetails>(); }));
        Properties.NotifyCustomizationModuleChanged();
        const auto SharedStyle = Style;
        auto& Outliner = FModuleManager::LoadModuleChecked<FSceneOutlinerModule>(TEXT("SceneOutliner"));
        Columns = Outliner.OnCreateActorBrowserColumns().AddLambda([SharedStyle](FSceneOutlinerInitializationOptions& Options,UWorld* World) {
            const FName LabelID = FSceneOutlinerBuiltInColumnTypes::Label();
            const auto* Label = Options.ColumnMap.Find(LabelID);
            const uint8 AfterLabel = Label ? FMath::Min<int32>(Label->PriorityIndex + 1,254) : 11;
            for (auto& Entry : Options.ColumnMap)
                if (Entry.Value.PriorityIndex >= AfterLabel && Entry.Value.PriorityIndex < 255) ++Entry.Value.PriorityIndex;
            Options.ColumnMap.Add(FOriginHierarchy::ID(), FSceneOutlinerColumnInfo(ESceneOutlinerColumnVisibility::Visible,AfterLabel,
                FCreateSceneOutlinerColumn::CreateLambda([](ISceneOutliner& View) { return MakeShared<FOriginHierarchy>(View); }),
                true,TOptional<float>(),FText::FromString(TEXT("Origin"))));
            const TWeakObjectPtr<UWorld> WeakWorld(World);
            Options.ModeFactory = FCreateSceneOutlinerMode::CreateLambda([WeakWorld,SharedStyle](SSceneOutliner* View) -> ISceneOutlinerMode* {
                return new FOriginBrowsingMode(View,WeakWorld,SharedStyle);
            });
        });
        if (FSlateApplication::IsInitialized())
        {
            HierarchyFeedbackProcessor = OriginHierarchyFeedback::CreateInputProcessor();
            FSlateApplication::Get().RegisterInputPreProcessor(HierarchyFeedbackProcessor);
        }
        bRegistered = true;
    }
    virtual void ShutdownModule() override
    {
        if (!bRegistered) return;
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        if (HierarchyFeedbackProcessor.IsValid() && FSlateApplication::IsInitialized())
            FSlateApplication::Get().UnregisterInputPreProcessor(HierarchyFeedbackProcessor);
        HierarchyFeedbackProcessor.Reset();
        OriginHierarchyFeedback::Shutdown();
        if (auto* Outliner = FModuleManager::GetModulePtr<FSceneOutlinerModule>(TEXT("SceneOutliner")))
            Outliner->OnCreateActorBrowserColumns().Remove(Columns);
        if (auto* Properties = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
            Properties->UnregisterCustomClassLayout(AOriginAnchor::StaticClass()->GetFName());
        if (Commands)
        {
            Commands->UnmapAction(FOriginCommands::Get().Create);
            Commands->UnmapAction(FOriginCommands::Get().Convert);
        }
        Commands.Reset();
        FOriginCommands::Unregister();
        FSlateStyleRegistry::UnRegisterSlateStyle(*Style);
        // Existing Outliner modes retain shared style ownership until destroyed.
        Style.Reset(); bRegistered = false;
    }
    virtual bool SupportsDynamicReloading() override { return false; }
};
IMPLEMENT_MODULE(FOriginEditorModule,OriginEditor)
