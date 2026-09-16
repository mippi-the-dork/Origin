#include "OriginOperations.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "Components/SceneComponent.h"
#include "Components/ActorComponent.h"
#include "LevelUtils.h"
#include "Components/PrimitiveComponent.h"
#include "Editor/GroupActor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "ActorFactories/ActorFactoryEmptyActor.h"
#include "AssetRegistry/AssetData.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace Origin
{
    bool IsEditorActor(const AActor* Actor)
    {
        return IsValid(Actor) && !Actor->IsTemplate() && Actor->GetWorld() &&
            Actor->GetWorld()->WorldType == EWorldType::Editor;
    }
    bool IsProtected(AActor* Actor)
    {
        if (!IsEditorActor(Actor)) return false;
        // Optional Focus interoperability: no link or plugin dependency. Read the
        // published saved-state key used by Focus's current implementation.
        const FString& State = Actor->GetPackage()->GetMetaData().GetValue(Actor, TEXT("Focus.SharedState.V1"));
        bool ValidState = State.Len() == 5;
        for (TCHAR Character : State) ValidState &= Character == TEXT('0') || Character == TEXT('1');
        return Actor->IsLockLocation() || (ValidState && (State[1] == TEXT('1') || State[2] == TEXT('1')));
    }
    void Notify(const FText& Message)
    {
        FNotificationInfo Info(Message); Info.ExpireDuration = 8.f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    TArray<AActor*> SelectedActors()
    {
        TArray<AActor*> Result;
        if (GEditor) for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            if (AActor* Actor = Cast<AActor>(*It); IsEditorActor(Actor)) Result.Add(Actor);
        return Result;
    }
    TArray<AActor*> DirectChildren(AActor* Actor)
    {
        TArray<AActor*> Result;
        if (IsEditorActor(Actor)) Actor->GetAttachedActors(Result);
        Result.RemoveAll([](AActor* A) { return !IsEditorActor(A); });
        Result.Sort([](const AActor& A, const AActor& B) { return A.GetActorLabel() < B.GetActorLabel(); });
        return Result;
    }
    void GatherBranch(AActor* Root, TArray<AActor*>& Out)
    {
        if (!IsEditorActor(Root) || Out.Contains(Root)) return;
        Out.Add(Root);
        for (AActor* Child : DirectChildren(Root)) GatherBranch(Child, Out);
    }
    static bool CheckActors(const TArray<AActor*>& Actors, FText& Reason)
    {
        for (AActor* Actor : Actors)
        {
            if (!IsEditorActor(Actor) || !Actor->GetRootComponent())
            { Reason = FText::FromString(TEXT("Every affected actor must be a loaded editor actor with a scene root.")); return false; }
            if (FLevelUtils::IsLevelLocked(Actor->GetLevel()))
            { Reason = FText::FromString(TEXT("An affected level is locked. Unlock it before changing this assembly.")); return false; }
            if (IsProtected(Actor))
            { Reason = FText::Format(NSLOCTEXT("Origin","Locked","Unlock {0} before changing this assembly."), FText::FromString(Actor->GetActorLabel())); return false; }
            if (Actor->GetActorScale3D().GetAbs().GetMin() < KINDA_SMALL_NUMBER)
            { Reason = FText::FromString(TEXT("An affected actor has zero scale. Restore a nonzero scale before editing the pivot.")); return false; }
            if (Actor->GetRootComponent()->IsSimulatingPhysics())
            { Reason = FText::FromString(TEXT("Turn off physics simulation on affected roots before changing the assembly.")); return false; }
            if (Actor->IsA<AGroupActor>() || Actor->GroupActor != nullptr)
            { Reason = FText::FromString(TEXT("Ungroup affected actors before creating or restructuring an Origin assembly. Hierarchy selection still supports folders and actors.")); return false; }
        }
        return true;
    }
    bool CanOperate(AOriginAnchor* Anchor, FText& Reason)
    {
        if (!GEditor || GEditor->PlayWorld)
        { Reason = FText::FromString(TEXT("Pivot editing is unavailable during PIE or simulation.")); return false; }
        if (!IsEditorActor(Anchor)) { Reason = FText::FromString(TEXT("Select an Origin Anchor in the editor world.")); return false; }
        TArray<AActor*> Actors; GatherBranch(Anchor, Actors);
        for (AActor* Parent = Anchor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
            Actors.AddUnique(Parent);
        return CheckActors(Actors, Reason);
    }
    static void ModifyActor(AActor* Actor)
    {
        Actor->Modify();
        if (Actor->GetRootComponent()) Actor->GetRootComponent()->Modify();
    }
    static void SelectOnly(const TArray<AActor*>& Actors)
    {
        GEditor->GetSelectedActors()->Modify();
        GEditor->SelectNone(false, true, false);
        for (AActor* Actor : Actors) if (IsEditorActor(Actor)) GEditor->SelectActor(Actor, true, false, true);
        GEditor->NoteSelectionChange();
    }
    static FBox AssemblyBounds(const TArray<AActor*>& Roots)
    {
        TArray<AActor*> All;
        for (AActor* Root : Roots) GatherBranch(Root, All);
        FBox Bounds(ForceInit);
        for (AActor* Actor : All)
        {
            // AABB of registered, non-visualization primitive components. Empty
            // actors contribute their pivot so an empty child is never ignored.
            FBox ActorBounds(ForceInit);
            Actor->ForEachComponent<UPrimitiveComponent>(false, [&ActorBounds](UPrimitiveComponent* C) {
                if (C->IsRegistered() && !C->IsVisualizationComponent()) ActorBounds += C->Bounds.GetBox();
            });
            if (ActorBounds.IsValid) Bounds += ActorBounds;
            else Bounds += Actor->GetActorLocation();
        }
        return Bounds;
    }
    static FVector BoundsPosition(const TArray<AActor*>& Roots, bool Bottom)
    {
        const FBox Bounds = AssemblyBounds(Roots);
        if (!Bounds.IsValid) return FVector::ZeroVector;
        FVector Result = Bounds.GetCenter();
        if (Bottom) Result.Z = Bounds.Min.Z;
        return Result;
    }
    FPivotSettings ReadSettings(const AOriginAnchor* A)
    {
        FPivotSettings S;
        if (A)
        {
            S.Mode=A->PivotMode; S.Child=A->PivotChild; S.Custom=A->CustomPivot;
            S.BoundsAxes=A->BoundsAxes; S.Reference=A->ReferenceActor;
            S.ComponentName=A->PivotComponentName; S.SocketName=A->PivotSocketName;
            S.Offset=A->OriginPivotOffset; S.bLocalOffset=A->bLocalPivotOffset;
        }
        return S;
    }
    static void WriteSettings(AOriginAnchor* A, const FPivotSettings& S)
    {
        A->PivotMode=S.Mode; A->PivotChild=S.Child; A->CustomPivot=S.Custom;
        A->BoundsAxes=S.BoundsAxes; A->ReferenceActor=S.Reference;
        A->PivotComponentName=S.ComponentName; A->PivotSocketName=S.SocketName;
        A->OriginPivotOffset=S.Offset; A->bLocalPivotOffset=S.bLocalOffset;
    }
    static bool SameSettings(const FPivotSettings& A, const FPivotSettings& B)
    {
        return A.Mode==B.Mode && A.Child==B.Child && A.Custom==B.Custom &&
            A.BoundsAxes==B.BoundsAxes && A.Reference==B.Reference &&
            A.ComponentName==B.ComponentName && A.SocketName==B.SocketName &&
            A.Offset==B.Offset && A.bLocalOffset==B.bLocalOffset;
    }
    // Read-only evaluation. Missing or unloaded references never move the pivot
    // to the origin and never cause synchronous actor/asset loading.
    static bool EvaluatePivot(AOriginAnchor* A, const FPivotSettings& S, FVector& Target, FText& Reason)
    {
        auto Fail=[&Reason](const TCHAR* Text) { Reason=FText::FromString(Text); return false; };
        const auto Children=DirectChildren(A);
        switch(S.Mode)
        {
        case EOriginPivotMode::BoundsCenter:
        case EOriginPivotMode::BottomCenter:
        case EOriginPivotMode::BoundsPoint:
        {
            if (Children.IsEmpty()) return Fail(TEXT("Add at least one child actor to calculate assembly bounds."));
            const FBox Bounds=AssemblyBounds(Children);
            if (!Bounds.IsValid) return Fail(TEXT("The assembly has no valid bounds."));
            Target=Bounds.GetCenter();
            if (S.Mode==EOriginPivotMode::BottomCenter) Target.Z=Bounds.Min.Z;
            if (S.Mode==EOriginPivotMode::BoundsPoint) for(int32 Axis=0;Axis<3;++Axis)
            {
                if (S.BoundsAxes[Axis]<0 || S.BoundsAxes[Axis]>2) return Fail(TEXT("Choose Min, Center or Max for each bounds axis."));
                Target[Axis]=S.BoundsAxes[Axis]==0 ? Bounds.Min[Axis] : S.BoundsAxes[Axis]==2 ? Bounds.Max[Axis] : Target[Axis];
            }
            break;
        }
        case EOriginPivotMode::AverageChildPivots:
            if (Children.IsEmpty()) return Fail(TEXT("Add at least one child actor to average child pivots."));
            Target=FVector::ZeroVector;
            for(AActor* Child:Children) Target+=Child->GetActorLocation();
            Target/=Children.Num(); break;
        case EOriginPivotMode::ChildActor:
        case EOriginPivotMode::ComponentSocket:
        {
            AActor* Child=S.Child.Get();
            if (!Children.Contains(Child)) return Fail(TEXT("Choose a loaded direct child as the pivot source."));
            Target=Child->GetActorLocation();
            if(S.Mode==EOriginPivotMode::ComponentSocket)
            {
                USceneComponent* Component=nullptr;
                TArray<USceneComponent*> Components; Child->GetComponents(Components);
                for(USceneComponent* C:Components) if(IsValid(C) && C->GetFName()==S.ComponentName) { Component=C; break; }
                if(!Component || !Component->IsRegistered()) return Fail(TEXT("Choose an available scene component on the selected child."));
                if(S.SocketName.IsNone()) Target=Component->GetComponentLocation();
                else
                {
                    if(!Component->DoesSocketExist(S.SocketName)) return Fail(TEXT("The selected socket is missing. Choose an available socket or Component Origin."));
                    Target=Component->GetSocketLocation(S.SocketName);
                }
            }
            break;
        }
        case EOriginPivotMode::ReferenceActor:
        {
            AActor* Reference=S.Reference.Get();
            if(!IsEditorActor(Reference) || Reference->GetWorld()!=A->GetWorld())
                return Fail(TEXT("Choose a loaded reference actor in the same editor world."));
            for(AActor* P=Reference;P;P=P->GetAttachParentActor())
                if(P==A) return Fail(TEXT("Choose a reference outside this assembly. Use Child Actor for a child pivot."));
            Target=Reference->GetActorLocation(); break;
        }
        case EOriginPivotMode::WorldOrigin: Target=FVector::ZeroVector; break;
        case EOriginPivotMode::Custom: Target=S.Custom; break;
        default: return Fail(TEXT("Choose a supported pivot mode."));
        }
        Target+=S.bLocalOffset ? A->GetActorQuat().RotateVector(S.Offset) : S.Offset;
        if(Target.ContainsNaN()) return Fail(TEXT("The calculated position is not finite. Check source transforms and offsets."));
        return true;
    }
    bool CanRecalculate(AOriginAnchor* A, FText& Reason)
    {
        if(!CanOperate(A,Reason)) return false;
        FVector Target;
        if(!EvaluatePivot(A,ReadSettings(A),Target,Reason)) return false;
        // Ignore numerical noise below 0.001 cm instead of offering no-op Undo entries.
        if(Target.Equals(A->GetActorLocation(),0.001))
        { Reason=FText::FromString(TEXT("The pivot is already up to date. No position change is needed.")); return false; }
        Reason=FText::FromString(TEXT("Update the pivot from its current source and offset, preserving child world transforms."));
        return true;
    }
    bool ApplySettings(AOriginAnchor* A, const FPivotSettings& S)
    {
        FText Reason;
        if(!CanOperate(A,Reason)) { Notify(Reason); return false; }
        if(S.Custom.ContainsNaN() || S.Offset.ContainsNaN())
        { Notify(FText::FromString(TEXT("Enter finite coordinates."))); return false; }
        const bool SettingsChanged=!SameSettings(ReadSettings(A),S);
        FVector Target;
        const bool ValidTarget=EvaluatePivot(A,S,Target,Reason);
        const bool Move=ValidTarget && !Target.Equals(A->GetActorLocation(),0.001);
        if(!SettingsChanged && !Move) return false;
        // Incomplete choices are saved so the user can first choose a mode,
        // then its actor/component/socket. Until valid, the assembly stays put.
        TArray<AActor*> Branch; GatherBranch(A,Branch);
        TArray<FTransform> Before;
        for(AActor* Actor:Branch) Before.Add(Actor->GetActorTransform());
        const FScopedTransaction Transaction(NSLOCTEXT("Origin","PivotTransaction","Origin: Change assembly pivot"));
        ModifyActor(A);
        if(Move) for(int32 I=1;I<Branch.Num();++I) ModifyActor(Branch[I]);
        WriteSettings(A,S);
        if(Move)
        {
            A->SetActorLocation(Target,false,nullptr,ETeleportType::TeleportPhysics);
            for(int32 I=1;I<Branch.Num();++I)
                Branch[I]->SetActorTransform(Before[I],false,nullptr,ETeleportType::TeleportPhysics);
            for(AActor* Actor:Branch) Actor->MarkPackageDirty();
            if(GUnrealEd && GEditor->GetSelectedActors()->IsSelected(A))
                GUnrealEd->UpdatePivotLocationForSelection(true);
            GEditor->RedrawLevelEditingViewports();
        }
        A->MarkPackageDirty();
        return true;
    }
    bool Recalculate(AOriginAnchor* A)
    {
        FText Reason;
        if(!CanRecalculate(A,Reason)) { Notify(Reason); return false; }
        return ApplySettings(A,ReadSettings(A));
    }
    bool SetPivot(AOriginAnchor* A, EOriginPivotMode Mode, AActor* Child, const FVector& Custom)
    {
        FPivotSettings S=ReadSettings(A); S.Mode=Mode; S.Child=Child; S.Custom=Custom;
        return ApplySettings(A,S);
    }
    void CreateAnchor()
    {
        if (!GEditor || GEditor->PlayWorld) return;
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return;
        auto Selected = SelectedActors();
        TArray<AActor*> Roots;
        for (AActor* A : Selected)
        {
            bool BelowSelection = false;
            for (AActor* P = A->GetAttachParentActor(); P; P = P->GetAttachParentActor())
                if (Selected.Contains(P)) { BelowSelection = true; break; }
            if (!BelowSelection) Roots.Add(A);
        }
        ULevel* Level = Roots.IsEmpty() ? World->GetCurrentLevel() : Roots[0]->GetLevel();
        AActor* Parent = Roots.IsEmpty() ? nullptr : Roots[0]->GetAttachParentActor();
        USceneComponent* ParentComponent = Roots.IsEmpty() ? nullptr : Roots[0]->GetRootComponent() ? Roots[0]->GetRootComponent()->GetAttachParent() : nullptr;
        const FName ParentSocket = Roots.IsEmpty() ? NAME_None : Roots[0]->GetAttachParentSocketName();
        TArray<AActor*> Affected;
        for (AActor* A : Roots)
        {
            if (A->GetLevel() != Level || A->GetAttachParentActor() != Parent ||
                A->GetAttachParentSocketName() != ParentSocket ||
                (A->GetRootComponent() && A->GetRootComponent()->GetAttachParent() != ParentComponent))
            { Notify(FText::FromString(TEXT("Select actors in one level with the same immediate attachment parent and socket."))); return; }
            GatherBranch(A, Affected);
        }
        for (AActor* P = Parent; P; P = P->GetAttachParentActor())
        {
            Affected.AddUnique(P);
            const FVector Scale = P->GetActorScale3D();
            if (!FMath::IsNearlyEqual(Scale.X,Scale.Y) || !FMath::IsNearlyEqual(Scale.Y,Scale.Z))
            { Notify(FText::FromString(TEXT("Creating an anchor beneath a non-uniformly scaled parent is not supported. Use a uniformly scaled parent to avoid transform decomposition changes."))); return; }
        }
        FText Reason;
        if (!CheckActors(Affected, Reason)) { Notify(Reason); return; }
        const FVector Position = Roots.IsEmpty() ? FVector::ZeroVector : BoundsPosition(Roots, false);
        FScopedTransaction Transaction(NSLOCTEXT("Origin","CreateTransaction","Origin: Create anchor"));
        AOriginAnchor* Anchor = Cast<AOriginAnchor>(GEditor->AddActor(Level, AOriginAnchor::StaticClass(), FTransform(Position), true, RF_Transactional, false));
        if (!Anchor) { Transaction.Cancel(); Notify(FText::FromString(TEXT("Unreal could not create the Origin Anchor."))); return; }
        bool HasStaticChild = false;
        for (AActor* A : Roots) HasStaticChild |= A->GetRootComponent()->Mobility == EComponentMobility::Static;
        Anchor->GetRootComponent()->SetMobility(HasStaticChild ? EComponentMobility::Static : EComponentMobility::Movable);
        // The tentative anchor is removed if engine attachment validation fails.
        bool Valid = !Parent || GEditor->CanParentActors(Parent, Anchor, &Reason);
        for (AActor* A : Roots) if (Valid) Valid = GEditor->CanParentActors(Anchor, A, &Reason);
        if (!Valid)
        {
            World->EditorDestroyActor(Anchor, true); Transaction.Cancel(); Notify(Reason); return;
        }
        for (AActor* A : Affected) ModifyActor(A);
        ModifyActor(Anchor);
        if (!Roots.IsEmpty()) Anchor->SetFolderPath(Roots[0]->GetFolderPath());
        Anchor->SetActorLabel(TEXT("Origin Anchor"));
        TArray<FTransform> OriginalWorld;
        for (AActor* A : Roots) OriginalWorld.Add(A->GetActorTransform());
        bool Attached = !ParentComponent || Anchor->AttachToComponent(ParentComponent, FAttachmentTransformRules::KeepWorldTransform, ParentSocket);
        for (int32 I = 0; I < Roots.Num() && Attached; ++I)
        {
            Attached = Roots[I]->AttachToActor(Anchor, FAttachmentTransformRules::KeepWorldTransform);
            Roots[I]->SetActorTransform(OriginalWorld[I],false,nullptr,ETeleportType::TeleportPhysics);
        }
        if (!Attached)
        {
            for (int32 I = 0; I < Roots.Num(); ++I)
            {
                Roots[I]->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                if (ParentComponent) Roots[I]->AttachToComponent(ParentComponent,FAttachmentTransformRules::KeepWorldTransform,ParentSocket);
                Roots[I]->SetActorTransform(OriginalWorld[I],false,nullptr,ETeleportType::TeleportPhysics);
            }
            World->EditorDestroyActor(Anchor,true); Transaction.Cancel();
            Notify(FText::FromString(TEXT("Attachment failed. The original actor hierarchy was restored."))); return;
        }
        Anchor->CustomPivot = Position;
        SelectOnly({Anchor});
        GEditor->RedrawLevelEditingViewports();
    }

    static bool HasUnsupportedReferences(AOriginAnchor* Anchor, FText& Reason)
    {
        // Native attachment bookkeeping and the level's actor array are expected.
        // Other persistent hard references need explicit review before replacement.
        TArray<FReferencerInformation> External;
        Anchor->RetrieveReferencers(nullptr, &External);
        for (const FReferencerInformation& Info : External)
        {
            UObject* Object = Info.Referencer;
            if (!Object || Object->HasAnyFlags(RF_Transient) || Object->IsIn(Anchor) ||
                Object->IsA<ULevel>() || Object->IsA<UWorld>() || Object->IsA<USelection>() ||
                Object->GetPackage() == GetTransientPackage()) continue;
            Reason = FText::Format(NSLOCTEXT("Origin","Referenced","{0} is referenced by {1}. Remove or redirect that reference before removing the anchor."),
                FText::FromString(Anchor->GetActorLabel()), FText::FromString(Object->GetPathName()));
            return true;
        }
        // Sequencer can bind through GUIDs rather than direct UObject pointers.
        // Fail conservatively when a sequence actor is present in this level.
        for (TActorIterator<AActor> It(Anchor->GetWorld()); It; ++It)
            for (UClass* Class = It->GetClass(); Class; Class = Class->GetSuperClass())
            if (Class->GetFName() == TEXT("LevelSequenceActor"))
            { Reason = FText::FromString(TEXT("This world contains a Level Sequence Actor. Sequence binding migration is not verified; remove/migrate sequence bindings before using anchor removal tools.")); return true; }
        return false;
    }
    void RemoveAnchors(const TArray<AOriginAnchor*>& Anchors, bool bConvert)
    {
        if (!GEditor || GEditor->PlayWorld || Anchors.IsEmpty()) return;
        FText Reason;
        for (AOriginAnchor* A : Anchors)
        {
            if (!CanOperate(A, Reason) || HasUnsupportedReferences(A, Reason)) { Notify(Reason); return; }
            // Conversion intentionally supports the exact native class only.
            // Arbitrary extra components would otherwise be discarded by the factory.
            TArray<UActorComponent*> Components; A->GetComponents(Components);
            if (A->GetClass() != AOriginAnchor::StaticClass() || Components.ContainsByPredicate([A](const UActorComponent* C) {
                return C && C != A->GetRootComponent() && !A->IsOriginEditorComponent(C);
            }))
            { Notify(FText::FromString(TEXT("Removal requires a plain Origin Anchor with only its scene root and built-in editor billboard. Move any added components to a separate actor first."))); return; }
            for (AActor* Parent = A->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
                if (Anchors.Contains(Cast<AOriginAnchor>(Parent)))
                { Notify(FText::FromString(TEXT("Remove nested anchors separately, starting with the deepest anchor."))); return; }
        }
        // Use the same public soft-reference query as Unreal's actor deletion
        // path. These references cannot be inferred from the attachment tree.
        TArray<FSoftObjectPath> Paths;
        for (AOriginAnchor* A : Anchors)
        {
            Paths.Add(FSoftObjectPath(A));
            Paths.Add(FSoftObjectPath(A->GetRootComponent()));
        }
        TMap<FSoftObjectPath,TArray<UObject*>> Referencers;
        {
            FScopedSlowTask SlowTask(1.f, NSLOCTEXT("Origin","References","Checking anchor references"));
            SlowTask.MakeDialog();
            FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get()
                .FindSoftReferencesToObjects(Paths, Referencers);
        }
        for (const auto& Pair : Referencers) for (UObject* Object : Pair.Value)
        {
            if (!IsValid(Object) || Object->HasAnyFlags(RF_Transient) || Object->GetPackage() == GetTransientPackage()) continue;
            bool Internal = false;
            for (AOriginAnchor* A : Anchors) Internal |= Object == A || Object->IsIn(A);
            if (!Internal)
            {
                Notify(FText::Format(NSLOCTEXT("Origin","SoftReference","Remove or redirect the soft reference in {0} before removing these anchors."),FText::FromString(Object->GetPathName())));
                return;
            }
        }
        if (bConvert && FMessageDialog::Open(EAppMsgType::YesNo,
            FText::FromString(TEXT("Convert the selected anchors to native empty Actors? Origin pivot settings will be removed. Native replacement preserves actor identity, but unloaded assets and custom reference systems cannot be audited here. Check external references before removing the plugin. Continue?"))) != EAppReturnType::Yes) return;
        UActorFactoryEmptyActor* Factory = bConvert ? NewObject<UActorFactoryEmptyActor>(GetTransientPackage()) : nullptr;
        if (Factory) Factory->bVisualizeActor = false;
        if (bConvert && !Factory) { Notify(FText::FromString(TEXT("Unreal's Empty Actor factory is unavailable."))); return; }
        const FScopedTransaction Transaction(NSLOCTEXT("Origin","RemoveTransaction","Origin: Remove anchors"));
        TArray<AActor*> OldActors, NewActors;
        TArray<TWeakObjectPtr<AActor>> PreservedChildren;
        TArray<FTransform> ChildWorldTransforms;
        TArray<FTransform> Transforms;
        TArray<bool> HiddenInGame, HiddenInEditor;
        TArray<FString> Labels;
        TArray<FFolder> Folders;
        for (AOriginAnchor* A : Anchors)
        {
            TArray<AActor*> Branch; GatherBranch(A, Branch);
            for (AActor* Actor : Branch)
            {
                ModifyActor(Actor);
                if (Actor != A) { PreservedChildren.Add(Actor); ChildWorldTransforms.Add(Actor->GetActorTransform()); }
            }
            if (A->GetAttachParentActor()) ModifyActor(A->GetAttachParentActor());
            HiddenInGame.Add(A->IsHidden()); HiddenInEditor.Add(A->IsTemporarilyHiddenInEditor());
            OldActors.Add(A); Transforms.Add(A->GetActorTransform()); Labels.Add(A->GetActorLabel()); Folders.Add(A->GetFolder());
        }
        if (bConvert)
        {
            // Engine replacement preserves name/GUID, reattaches children and
            // redirects its supported level references. No Origin class survives.
            UEditorActorSubsystem::ReplaceActors(Factory, FAssetData(AActor::StaticClass()), OldActors, &NewActors, false);
            for (AActor* New : NewActors)
            {
                const int32 Index = OldActors.IndexOfByPredicate([New](AActor* Old) { return Old->GetActorGuid() == New->GetActorGuid(); });
                if (Index != INDEX_NONE)
                {
                    New->SetActorHiddenInGame(HiddenInGame[Index]);
                    New->SetIsTemporarilyHiddenInEditor(HiddenInEditor[Index]);
                    New->SetActorEnableCollision(false);
                    New->SetActorLabel(Labels[Index]); New->SetFolderPath(Folders[Index].GetPath());
                    New->SetActorTransform(Transforms[Index], false, nullptr, ETeleportType::TeleportPhysics);
                }
            }
            if (NewActors.Num() != Anchors.Num()) Notify(FText::FromString(TEXT("Unreal could not replace every anchor. Inspect the result and Undo before continuing.")));
        }
        else
        {
            for (AOriginAnchor* A : Anchors)
            {
                for (AActor* Child : DirectChildren(A))
                {
                    Child->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                    NewActors.Add(Child);
                }
                A->GetWorld()->EditorDestroyActor(A, true);
            }
        }
        // Replacement honors a user preference about scale. Reassert the
        // original assembly transforms after restoring each replacement root.
        for (int32 I=0;I<PreservedChildren.Num();++I) if (auto* Child=PreservedChildren[I].Get())
            Child->SetActorTransform(ChildWorldTransforms[I],false,nullptr,ETeleportType::TeleportPhysics);
        SelectOnly(NewActors); GEditor->RedrawLevelEditingViewports();
    }
    void ConvertSelected()
    {
        TArray<AOriginAnchor*> Anchors;
        for (AActor* A : SelectedActors()) if (auto* Anchor = Cast<AOriginAnchor>(A)) Anchors.Add(Anchor);
        if (Anchors.IsEmpty()) Notify(FText::FromString(TEXT("Select one or more Origin Anchors.")));
        else RemoveAnchors(Anchors, true);
    }
}
