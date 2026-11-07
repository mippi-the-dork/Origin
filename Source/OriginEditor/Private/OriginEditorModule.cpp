// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "OriginOperations.h"
#include "OriginActorAssemblyAuthoring.h"
#include "OriginPerformance.h"
#include "OriginDetails.h"
#include "OriginPivotComponentDetails.h"
#include "OriginPivotComponent.h"
#include "OriginPivotViewportMode.h"
#include "EditorModeRegistry.h"
#include "OriginHierarchy.h"
#include "OriginHierarchyFeedback.h"
#include "SceneOutlinerModule.h"
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

class FOriginCommands : public TCommands<FOriginCommands>
{
public:
    FOriginCommands() : TCommands<FOriginCommands>(TEXT("Origin"), NSLOCTEXT("Origin","Commands","Origin"), NAME_None, FAppStyle::GetAppStyleSetName()) {}
    TSharedPtr<FUICommandInfo> Create, Convert, InsertActorPivot, AdoptActors, PromoteActors, DetachActors;
    virtual void RegisterCommands() override
    {
#define LOCTEXT_NAMESPACE "Origin"
        // Deliberately unbound: users can assign a chord in Editor Preferences
        // without replacing Unreal's grouping shortcut or another plugin's key.
        UI_COMMAND(Create,"Create Origin Anchor","Create an anchor above the selected actors, preserving their world transforms. With no actors selected, create an empty anchor at world origin.",EUserInterfaceActionType::Button,FInputChord());
        UI_COMMAND(Convert,"Convert Selected Origin Anchors","Replace selected anchors with native empty Actors while preserving the assembly.",EUserInterfaceActionType::Button,FInputChord());
        UI_COMMAND(InsertActorPivot,"Insert Origin Anchor Above Actor","Insert an Origin Anchor between one selected Actor and its current attachment parent. Preserves parent socket and world transforms.",EUserInterfaceActionType::Button,FInputChord());
        UI_COMMAND(AdoptActors,"Reparent Actors to Origin Anchor","Select exactly one Origin Anchor and one or more other Actors; make those Actors direct children of the Anchor while preserving world transforms.",EUserInterfaceActionType::Button,FInputChord());
        UI_COMMAND(PromoteActors,"Promote Actors From Origin Anchor","Move selected direct children of one Origin Anchor to that Anchor's parent, keeping world transforms. The Anchor remains.",EUserInterfaceActionType::Button,FInputChord());
        UI_COMMAND(DetachActors,"Detach Actors From Origin Anchor","Detach selected direct children of one Origin Anchor to the world root, keeping world transforms. The Anchor remains.",EUserInterfaceActionType::Button,FInputChord());
#undef LOCTEXT_NAMESPACE
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
            Section.AddMenuEntryWithCommandList(FOriginCommands::Get().InsertActorPivot,Commands);
            Section.AddMenuEntryWithCommandList(FOriginCommands::Get().AdoptActors,Commands);
            Section.AddMenuEntryWithCommandList(FOriginCommands::Get().PromoteActors,Commands);
            Section.AddMenuEntryWithCommandList(FOriginCommands::Get().DetachActors,Commands);
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
        FEditorModeRegistry::Get().RegisterMode<FOriginPivotViewportMode>(
            FOriginPivotViewportMode::ModeID,
            FText::FromString(TEXT("Origin Pivot Edit")),
            FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("LevelEditor.Tabs.Modes")),
            false, 900);
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
        // Alpha 5.2 Actor assembly commands deliberately use Outliner selection rather
        // than a Details-only target: a target Anchor and multiple Actors can be selected together.
        auto SelectedForAdopt = []() {
            TArray<AActor*> Selected = Origin::SelectedActors();
            AOriginAnchor* Anchor = nullptr;
            for (AActor* Actor : Selected) if (AOriginAnchor* Candidate = Cast<AOriginAnchor>(Actor))
            {
                if (Anchor) return TPair<AOriginAnchor*, TArray<AActor*>>(nullptr, TArray<AActor*>());
                Anchor = Candidate;
            }
            if (!Anchor) return TPair<AOriginAnchor*, TArray<AActor*>>(nullptr, TArray<AActor*>());
            Selected.Remove(Anchor);
            return TPair<AOriginAnchor*, TArray<AActor*>>(Anchor, MoveTemp(Selected));
        };
        Commands->MapAction(FOriginCommands::Get().InsertActorPivot,
            FExecuteAction::CreateLambda([] {
                const TArray<AActor*> Selected = Origin::SelectedActors();
                FText Reason;
                AOriginAnchor* NewAnchor = nullptr;
                if (Selected.Num() != 1 || !OriginActorAssembly::Insert(Selected[0], NewAnchor, Reason))
                    Origin::Notify(Selected.Num() == 1 ? Reason : FText::FromString(TEXT("Select exactly one Actor to insert an Origin Anchor above it.")));
                else { GEditor->SelectNone(false, true, false); GEditor->SelectActor(NewAnchor, true, true, true); }
            }),
            FCanExecuteAction::CreateLambda([] { return GEditor && !GEditor->PlayWorld && Origin::SelectedActors().Num() == 1; }));
        Commands->MapAction(FOriginCommands::Get().AdoptActors,
            FExecuteAction::CreateLambda([SelectedForAdopt] {
                auto Pair = SelectedForAdopt();
                FText Reason;
                if (!OriginActorAssembly::Adopt(Pair.Key, Pair.Value, Reason)) Origin::Notify(Reason);
            }),
            FCanExecuteAction::CreateLambda([SelectedForAdopt] {
                if (!GEditor || GEditor->PlayWorld) return false;
                auto Pair = SelectedForAdopt();
                return Pair.Key && !Pair.Value.IsEmpty();
            }));
        Commands->MapAction(FOriginCommands::Get().PromoteActors,
            FExecuteAction::CreateLambda([] {
                FText Reason;
                if (!OriginActorAssembly::Promote(Origin::SelectedActors(), Reason)) Origin::Notify(Reason);
            }),
            FCanExecuteAction::CreateLambda([] {
                if (!GEditor || GEditor->PlayWorld) return false;
                const TArray<AActor*> Actors = Origin::SelectedActors();
                return !Actors.IsEmpty() && Cast<AOriginAnchor>(Actors[0]->GetAttachParentActor()) != nullptr;
            }));
        Commands->MapAction(FOriginCommands::Get().DetachActors,
            FExecuteAction::CreateLambda([] {
                FText Reason;
                if (!OriginActorAssembly::Detach(Origin::SelectedActors(), Reason)) Origin::Notify(Reason);
            }),
            FCanExecuteAction::CreateLambda([] {
                if (!GEditor || GEditor->PlayWorld) return false;
                const TArray<AActor*> Actors = Origin::SelectedActors();
                return !Actors.IsEmpty() && Cast<AOriginAnchor>(Actors[0]->GetAttachParentActor()) != nullptr;
            }));
        auto& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        LevelCommands = LevelEditor.GetGlobalLevelEditorActions();
        LevelEditor.GetGlobalLevelEditorActions()->Append(Commands.ToSharedRef());
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,&FOriginEditorModule::RegisterMenus));
        auto& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
        Properties.RegisterCustomClassLayout(AOriginAnchor::StaticClass()->GetFName(),FOnGetDetailCustomizationInstance::CreateLambda([] { return MakeShared<FOriginDetails>(); }));
        Properties.RegisterCustomClassLayout(UOriginPivotComponent::StaticClass()->GetFName(),FOnGetDetailCustomizationInstance::CreateLambda([] { return MakeShared<FOriginPivotComponentDetails>(); }));
        Properties.NotifyCustomizationModuleChanged();
        auto& Outliner = FModuleManager::LoadModuleChecked<FSceneOutlinerModule>(TEXT("SceneOutliner"));
        // Register only Origin's own column. Actor browsing, world selection,
        // filters and PIE/SIE behavior remain entirely owned by Unreal.
        Columns = Outliner.OnCreateActorBrowserColumns().AddLambda([](FSceneOutlinerInitializationOptions& Options, UWorld*) {
            const FName LabelID = FSceneOutlinerBuiltInColumnTypes::Label();
            const auto* Label = Options.ColumnMap.Find(LabelID);
            const uint8 AfterLabel = Label ? FMath::Min<int32>(Label->PriorityIndex + 1,254) : 11;
            for (auto& Entry : Options.ColumnMap)
                if (Entry.Value.PriorityIndex >= AfterLabel && Entry.Value.PriorityIndex < 255) ++Entry.Value.PriorityIndex;
            Options.ColumnMap.Add(FOriginHierarchy::ID(), FSceneOutlinerColumnInfo(ESceneOutlinerColumnVisibility::Visible,AfterLabel,
                FCreateSceneOutlinerColumn::CreateLambda([](ISceneOutliner& View) { return MakeShared<FOriginHierarchy>(View); }),
                true,TOptional<float>(),FText::FromString(TEXT("Origin"))));
        });
        if (FSlateApplication::IsInitialized())
        {
            HierarchyFeedbackProcessor = OriginHierarchyFeedback::CreateInputProcessor();
            FSlateApplication::Get().RegisterInputPreProcessor(HierarchyFeedbackProcessor);
        }
        OriginPerformance::Initialize();
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
        OriginPerformance::Shutdown();
        FOriginPivotViewportMode::Shutdown();
        FEditorModeRegistry::Get().UnregisterMode(FOriginPivotViewportMode::ModeID);
        if (auto* Outliner = FModuleManager::GetModulePtr<FSceneOutlinerModule>(TEXT("SceneOutliner")))
            Outliner->OnCreateActorBrowserColumns().Remove(Columns);
        if (auto* Properties = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
            {
                Properties->UnregisterCustomClassLayout(AOriginAnchor::StaticClass()->GetFName());
                Properties->UnregisterCustomClassLayout(UOriginPivotComponent::StaticClass()->GetFName());
            }
        if (Commands)
        {
            Commands->UnmapAction(FOriginCommands::Get().Create);
            Commands->UnmapAction(FOriginCommands::Get().Convert);
            Commands->UnmapAction(FOriginCommands::Get().InsertActorPivot);
            Commands->UnmapAction(FOriginCommands::Get().AdoptActors);
            Commands->UnmapAction(FOriginCommands::Get().PromoteActors);
            Commands->UnmapAction(FOriginCommands::Get().DetachActors);
        }
        Commands.Reset();
        FOriginCommands::Unregister();
        FSlateStyleRegistry::UnRegisterSlateStyle(*Style);
        Style.Reset(); bRegistered = false;
    }
    virtual bool SupportsDynamicReloading() override { return false; }
};
IMPLEMENT_MODULE(FOriginEditorModule,OriginEditor)
