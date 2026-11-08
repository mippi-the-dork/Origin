// Copyright Mippithedork 2026, Inc. All Rights Reserved.

#include "OriginPivotAuthoring.h"
#include "OriginAnchor.h"

#include "OriginBlueprintPivotEdit.h"
#include "OriginPivotComponent.h"
#include "OriginPivotEditSession.h"
#include "OriginPivotSnapMath.h"
#include "OriginPivotViewportMode.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Selection.h"

#define LOCTEXT_NAMESPACE "OriginPivotAuthoring"

namespace
{
    void NotifyChanged(UOriginPivotComponent* Pivot, UBlueprint* Blueprint)
    {
        Pivot->MarkPackageDirty();
        if (Blueprint)
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            Blueprint->MarkPackageDirty();
        }
        else if (AActor* Actor = Pivot->GetOwner())
        {
            Actor->MarkPackageDirty();
            if (GUnrealEd && GEditor && GEditor->GetSelectedComponents() &&
                GEditor->GetSelectedComponents()->IsSelected(Pivot))
            {
                GUnrealEd->UpdatePivotLocationForSelection(false);
            }
            if (GEditor) GEditor->RedrawLevelEditingViewports();
        }
    }

    bool ResolveAuthored(UOriginPivotComponent* Pivot, UBlueprint*& OutBP, USCS_Node*& OutNode)
    {
        OutBP = nullptr;
        OutNode = nullptr;
        return Pivot && Pivot->IsTemplate() && OriginBlueprintPivot::Resolve(Pivot, OutBP, OutNode);
    }

    void ModifyAssembly(UOriginPivotComponent* Pivot, UBlueprint* BP, USCS_Node* Node)
    {
        if (BP)
        {
            BP->Modify();
            if (BP->SimpleConstructionScript) BP->SimpleConstructionScript->Modify();
            if (Node) Node->Modify();
            if (Node)
            {
                for (USCS_Node* ChildNode : Node->GetChildNodes())
                {
                    if (ChildNode)
                    {
                        ChildNode->Modify();
                        if (USceneComponent* Child = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()))
                            Child->Modify();
                    }
                }
            }
        }
        else
        {
            TArray<USceneComponent*> Children;
            Pivot->GetChildrenComponents(true, Children);
            TSet<AActor*> Owners;
            if (AActor* Actor = Pivot->GetOwner()) Owners.Add(Actor);
            for (USceneComponent* Child : Children)
            {
                if (IsValid(Child))
                {
                    Child->Modify();
                    if (AActor* Owner = Child->GetOwner()) Owners.Add(Owner);
                }
            }
            for (AActor* Owner : Owners) if (IsValid(Owner)) Owner->Modify();
        }
        Pivot->Modify();
    }

    bool ApplyCandidate(UOriginPivotComponent* Pivot, const FTransform& Candidate,
        FName NewActiveSlot, bool bChangeActive, const FText& TransactionLabel, FText& OutReason)
    {
        if (!FOriginPivotAuthoring::CanAuthor(Pivot))
        {
            OutReason = LOCTEXT("Unavailable", "Select a movable Origin Pivot in a level or an authored Blueprint component. Finish active viewport editing first.");
            return false;
        }
        UBlueprint* BP = nullptr;
        USCS_Node* Node = nullptr;
        ResolveAuthored(Pivot, BP, Node);

        // Validate the full authored hierarchy before opening a transaction.
        if (BP)
        {
            OriginBlueprintPivot::FPlan Plan;
            if (!OriginBlueprintPivot::Plan(Pivot, Candidate, Plan, OutReason)) return false;
        }
        else
        {
            const EOriginPivotResult Preflight = Pivot->CanSetOriginPivotWorldTransform(Candidate, OutReason);
            if (Preflight != EOriginPivotResult::Success && Preflight != EOriginPivotResult::NoChange)
                return false;
        }

        const bool bPoseSame = BP
            ? Pivot->GetRelativeTransform().Equals(Candidate, 1.e-5)
            : Pivot->GetComponentTransform().Equals(Candidate, 1.e-5);
        const bool bSlotSame = !bChangeActive || Pivot->OriginPivotConfiguration.ActiveSlotName == NewActiveSlot;
        if (bPoseSame && bSlotSame) { OutReason = FText::GetEmpty(); return true; }

        FScopedTransaction Transaction(TransactionLabel);
        ModifyAssembly(Pivot, BP, Node);
        bool bApplied = true;
        if (!bPoseSame)
        {
            if (BP)
            {
                // Outer transaction owns Undo; session preserves original authored
                // child transforms and synchronizes the Blueprint preview.
                FOriginBlueprintPivotEditSession Session(Pivot, false);
                bApplied = Session.Apply(Candidate, OutReason);
                if (bApplied) Session.Finish();
                else Session.Cancel();
            }
            else
            {
                FOriginPivotEditSession Session(Pivot, false);
                const EOriginPivotResult Result = Session.Apply(Candidate, OutReason);
                bApplied = Result == EOriginPivotResult::Success || Result == EOriginPivotResult::NoChange;
                if (bApplied) Session.Finish();
                else Session.Cancel();
            }
        }
        if (!bApplied)
        {
            Transaction.Cancel();
            return false;
        }
        if (bChangeActive) Pivot->OriginPivotConfiguration.ActiveSlotName = NewActiveSlot;
        NotifyChanged(Pivot, BP);
        OutReason = FText::GetEmpty();
        return true;
    }

    bool EditSlot(UOriginPivotComponent* Pivot, FName Name, bool bCreate, bool bDelete, FText& Reason)
    {
        if (!FOriginPivotAuthoring::CanAuthor(Pivot))
        {
            Reason = LOCTEXT("NotEditable", "Pivot slot authoring is unavailable in this context or while viewport editing is active.");
            return false;
        }
        // Validate names and operations before opening the Undo transaction.
        if (Name.IsNone() || Name.ToString().TrimStartAndEnd().IsEmpty())
        {
            Reason = LOCTEXT("EmptyName", "Enter a nonempty pivot slot name.");
            return false;
        }
        const FOriginPivotSlot* Existing = Pivot->OriginPivotConfiguration.FindSlot(Name);
        if ((bCreate && Existing) || (!bCreate && !Existing) ||
            (bDelete && Name == FName(TEXT("Default"))))
        {
            Reason = LOCTEXT("InvalidOperation", "That slot cannot be created, updated, or deleted. Check the name; Default cannot be deleted.");
            return false;
        }
        UBlueprint* BP = nullptr;
        USCS_Node* Node = nullptr;
        ResolveAuthored(Pivot, BP, Node);
        FScopedTransaction Transaction(bDelete
            ? LOCTEXT("RemoveSlot", "Origin: Delete Pivot Slot")
            : bCreate ? LOCTEXT("AddSlot", "Origin: Create Pivot Slot")
                      : LOCTEXT("UpdateSlot", "Origin: Capture Pivot Slot"));
        if (BP) BP->Modify();
        Pivot->Modify();
        const bool bOk = bDelete ? Pivot->DeleteOriginPivotSlot(Name, Reason)
            : bCreate ? Pivot->CreateOriginPivotSlot(Name, Reason)
                      : Pivot->CaptureOriginPivotSlot(Name, Reason);
        if (!bOk) { Transaction.Cancel(); return false; }
        NotifyChanged(Pivot, BP);
        return true;
    }
}

bool FOriginPivotAuthoring::CanAuthor(UOriginPivotComponent* Pivot)
{
    if (!IsValid(Pivot) || !GEditor || GEditor->PlayWorld ||
        FOriginPivotViewportMode::IsEditing(Pivot)) return false;
    if (Pivot->IsTemplate())
    {
        UBlueprint* BP = nullptr;
        USCS_Node* Node = nullptr;
        return ResolveAuthored(Pivot, BP, Node);
    }
    return Pivot->IsRegistered() && IsValid(Pivot->GetOwner()) &&
        Pivot->GetWorld() && Pivot->GetWorld()->WorldType == EWorldType::Editor &&
        Pivot->Mobility == EComponentMobility::Movable;
}

bool FOriginPivotAuthoring::CreateSlot(UOriginPivotComponent* Pivot, FName Name, FText& Reason)
{ return EditSlot(Pivot, Name, true, false, Reason); }

bool FOriginPivotAuthoring::CaptureSlot(UOriginPivotComponent* Pivot, FName Name, FText& Reason)
{ return EditSlot(Pivot, Name, false, false, Reason); }

bool FOriginPivotAuthoring::RemoveSlot(UOriginPivotComponent* Pivot, FName Name, FText& Reason)
{ return EditSlot(Pivot, Name, false, true, Reason); }

namespace
{
    bool CopyOrRenameSlot(UOriginPivotComponent* Pivot, FName Source, FName NewName,
        bool bRename, FText& Reason)
    {
        if (!FOriginPivotAuthoring::CanAuthor(Pivot))
        {
            Reason = LOCTEXT("RenameUnavailable", "Pivot slot authoring is not available in the current editor context.");
            return false;
        }
        if (NewName.IsNone() || NewName.ToString().TrimStartAndEnd().IsEmpty() ||
            !Pivot->OriginPivotConfiguration.FindSlot(Source) ||
            Pivot->OriginPivotConfiguration.FindSlot(NewName) ||
            (bRename && Source == FName(TEXT("Default"))))
        {
            Reason = LOCTEXT("RenameConflict", "Enter a unique name. The Default slot cannot be renamed.");
            return false;
        }
        UBlueprint* BP = nullptr;
        USCS_Node* Node = nullptr;
        ResolveAuthored(Pivot, BP, Node);
        FScopedTransaction Transaction(bRename
            ? LOCTEXT("RenameTransaction", "Origin: Rename Pivot Slot")
            : LOCTEXT("DuplicateTransaction", "Origin: Duplicate Pivot Slot"));
        if (BP) BP->Modify();
        Pivot->Modify();
        const bool bOk = bRename
            ? Pivot->RenameOriginPivotSlot(Source, NewName, Reason)
            : Pivot->DuplicateOriginPivotSlot(Source, NewName, Reason);
        if (!bOk) { Transaction.Cancel(); return false; }
        NotifyChanged(Pivot, BP);
        return true;
    }
}

bool FOriginPivotAuthoring::DuplicateSlot(UOriginPivotComponent* Pivot, FName Source,
    FName NewName, FText& Reason)
{ return CopyOrRenameSlot(Pivot, Source, NewName, false, Reason); }

bool FOriginPivotAuthoring::RenameSlot(UOriginPivotComponent* Pivot, FName OldName,
    FName NewName, FText& Reason)
{ return CopyOrRenameSlot(Pivot, OldName, NewName, true, Reason); }

bool FOriginPivotAuthoring::ActivateSlot(UOriginPivotComponent* Pivot, FName Name, FText& Reason)
{
    if (!CanAuthor(Pivot))
    {
        Reason = LOCTEXT("SlotNotEditable", "Select an editable Origin Pivot and finish viewport editing first.");
        return false;
    }
    FTransform StoredRelative;
    if (!Pivot->GetOriginPivotSlotPose(Name, StoredRelative))
    {
        Reason = LOCTEXT("Uncaptured", "This slot has no captured pose. Select it and use Capture Current before activating.");
        return false;
    }
    const FOriginPivotSlot* Slot = Pivot->OriginPivotConfiguration.FindSlot(Name);
    const FName Socket = Pivot->GetAttachSocketName();
    if (!Slot || Slot->CapturedSocketName != Socket)
    {
        Reason = LOCTEXT("SlotSocketChanged", "The pivot attachment socket changed since capture. Recapture the slot in its current frame.");
        return false;
    }
    FTransform Candidate = StoredRelative;
    if (!Pivot->IsTemplate())
    {
        if (const USceneComponent* Parent = Pivot->GetAttachParent())
        {
            if (!Socket.IsNone() && !Parent->DoesSocketExist(Socket))
            {
                Reason = LOCTEXT("SlotSocketMissing", "The pivot attachment socket no longer exists. Recapture after choosing a valid socket.");
                return false;
            }
            Candidate = StoredRelative * Parent->GetSocketTransform(Socket, RTS_World);
        }
        else
        {
            Reason = LOCTEXT("SlotNeedsParent", "Root instances have no stable parent-local slot frame. Add a Scene Component root above Origin Pivot before activating slots.");
            return false;
        }
    }
    return ApplyCandidate(Pivot, Candidate, Name, true,
        LOCTEXT("ActivateSlot", "Origin: Activate Pivot Slot (Preserve Children)"), Reason);
}

bool FOriginPivotAuthoring::ComputeSnapCandidate(UOriginPivotComponent* Pivot,
    EOriginPivotSnapAction Action, FTransform& OutCandidate, FText& Reason)
{
    if (!CanAuthor(Pivot))
    {
        Reason = LOCTEXT("SnapNotEditable", "This Origin Pivot cannot be edited in the current editor context.");
        return false;
    }
    const bool bBlueprint = Pivot->IsTemplate();
    OutCandidate = bBlueprint ? Pivot->GetRelativeTransform() : Pivot->GetComponentTransform();
    if (Action == EOriginPivotSnapAction::ParentOrigin ||
        Action == EOriginPivotSnapAction::ParentOrientation)
    {
        FTransform ParentFrame = FTransform::Identity;
        if (!bBlueprint)
        {
            if (const USceneComponent* Parent = Pivot->GetAttachParent())
            {
                const FName Socket = Pivot->GetAttachSocketName();
                if (!Socket.IsNone() && !Parent->DoesSocketExist(Socket))
                {
                    Reason = LOCTEXT("SnapParentSocketMissing", "The pivot parent attachment socket no longer exists.");
                    return false;
                }
                ParentFrame = Parent->GetSocketTransform(Socket, RTS_World);
            }
        }
        else if (!Pivot->GetAttachSocketName().IsNone())
        {
            Reason = LOCTEXT("SnapAuthoredSocket", "A Blueprint pivot attached to a socket needs an evaluated parent socket frame. Use the live instance or remove this authored attachment socket.");
            return false;
        }
        if (Action == EOriginPivotSnapAction::ParentOrigin)
            OutCandidate.SetTranslation(ParentFrame.GetLocation());
        else
            OutCandidate.SetRotation(ParentFrame.GetRotation());
    }
    else
    {
        TArray<FTransform> ChildRelativeTransforms;
        FTransform PivotFrame = OutCandidate;
        if (bBlueprint)
        {
            UBlueprint* BP = nullptr;
            USCS_Node* Node = nullptr;
            if (!ResolveAuthored(Pivot, BP, Node)) return false;
            for (USCS_Node* ChildNode : Node->GetChildNodes())
            {
                const USceneComponent* Child = ChildNode
                    ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
                if (!IsValid(Child))
                {
                    Reason = LOCTEXT("InvalidChild", "An authored child lacks a valid Scene Component template.");
                    return false;
                }
                ChildRelativeTransforms.Add(Child->GetRelativeTransform());
            }
        }
        else
        {
            // Use actual world positions (not implicit relative transforms) so
            // absolute-location child attachments also snap to the right place.
            PivotFrame = FTransform::Identity;
            TArray<USceneComponent*> Children;
            Pivot->GetChildrenComponents(false, Children);
            for (USceneComponent* Child : Children)
            {
                if (!IsValid(Child)) continue;
                FTransform ChildWorldPosition = FTransform::Identity;
                ChildWorldPosition.SetTranslation(Child->GetComponentLocation());
                ChildRelativeTransforms.Add(ChildWorldPosition);
            }
        }
        FVector Center;
        if (!FOriginPivotSnapMath::AverageChildOrigins(PivotFrame, ChildRelativeTransforms, Center))
        {
            Reason = LOCTEXT("NoChildren", "No valid direct child origins were found, or a child has an invalid transform.");
            return false;
        }
        OutCandidate.SetTranslation(Center);
    }
    if (!OutCandidate.IsValid() || OutCandidate.ContainsNaN())
    {
        Reason = LOCTEXT("InvalidSnap", "The calculated snap pose is invalid.");
        return false;
    }
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginPivotAuthoring::SnapPivot(UOriginPivotComponent* Pivot, EOriginPivotSnapAction Action, FText& Reason)
{
    FTransform Candidate;
    if (!ComputeSnapCandidate(Pivot, Action, Candidate, Reason)) return false;
    return ApplyCandidate(Pivot, Candidate, NAME_None, false,
        LOCTEXT("SnapPivot", "Origin: Snap / Align Pivot (Preserve Children)"), Reason);
}


namespace
{
    bool SameDefinition(const FOriginPivotDefinition& A, const FOriginPivotDefinition& B)
    {
        return A.PositionMode == B.PositionMode && A.ChildActor == B.ChildActor &&
            A.ReferenceActor == B.ReferenceActor && A.CustomPosition == B.CustomPosition &&
            A.BoundsAxes == B.BoundsAxes && A.ComponentName == B.ComponentName &&
            A.SocketName == B.SocketName && A.Offset == B.Offset &&
            A.bLocalOffset == B.bLocalOffset;
    }

    // Gather actual primitive geometry for one direct child branch. Empty
    // SceneComponents are authoring handles, NOT geometry; using every such
    // origin in a mixed mesh branch artificially expands the assembly bounds.
    void GatherAuthoringGeometry(const USCS_Node* ParentNode, const FTransform& ParentInFrame,
        FBox& OutGeometry, bool& bUnsupported)
    {
        if (!ParentNode) return;
        for (const USCS_Node* ChildNode : ParentNode->GetChildNodes())
        {
            const USceneComponent* Child = ChildNode
                ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
            if (!IsValid(Child) || Child->IsUsingAbsoluteLocation() ||
                Child->IsUsingAbsoluteRotation() || Child->IsUsingAbsoluteScale() ||
                !Child->GetAttachSocketName().IsNone())
            { bUnsupported = true; return; }
            const FTransform ChildInFrame = Child->GetRelativeTransform() * ParentInFrame;
            if (!ChildInFrame.IsValid() || ChildInFrame.ContainsNaN())
            { bUnsupported = true; return; }
            if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child))
            {
                const FBox Bounds = Primitive->CalcBounds(ChildInFrame).GetBox();
                if (Bounds.IsValid) OutGeometry += Bounds;
            }
            GatherAuthoringGeometry(ChildNode, ChildInFrame, OutGeometry, bUnsupported);
            if (bUnsupported) return;
        }
    }

    void GatherLiveGeometry(const USceneComponent* Root, FBox& OutGeometry, bool& bUnsupported)
    {
        if (!IsValid(Root) || !Root->IsRegistered() || Root->GetComponentTransform().ContainsNaN())
        { bUnsupported = true; return; }
        if (!Root->IsVisualizationComponent())
            if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Root))
            {
                const FBox Bounds = Primitive->Bounds.GetBox();
                if (Bounds.IsValid) OutGeometry += Bounds;
            }
        TArray<USceneComponent*> Children;
        Root->GetChildrenComponents(false, Children);
        for (const USceneComponent* Child : Children)
        {
            if (IsValid(Child) && Child->IsVisualizationComponent()) continue;
            GatherLiveGeometry(Child, OutGeometry, bUnsupported);
            if (bUnsupported) return;
        }
    }

    USceneComponent* DirectSource(UOriginPivotComponent* Pivot, FName Name)
    {
        if (!Pivot || Name.IsNone()) return nullptr;
        if (Pivot->IsTemplate())
        {
            UBlueprint* BP = nullptr; USCS_Node* Node = nullptr;
            if (!ResolveAuthored(Pivot, BP, Node)) return nullptr;
            for (USCS_Node* ChildNode : Node->GetChildNodes())
            {
                USceneComponent* Child = ChildNode
                    ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
                if (IsValid(Child) && (ChildNode->GetVariableName() == Name || Child->GetFName() == Name))
                    return Child;
            }
            return nullptr;
        }
        TArray<USceneComponent*> Children;
        Pivot->GetChildrenComponents(false, Children);
        for (USceneComponent* Child : Children)
            if (IsValid(Child) && !Child->IsVisualizationComponent() && Child->GetFName() == Name)
                return Child;
        return nullptr;
    }
}

FOriginPivotDefinition FOriginPivotAuthoring::ReadSourceDefinition(const UOriginPivotComponent* Pivot)
{
    if (Pivot)
        if (const FOriginPivotSlot* Default = Pivot->OriginPivotConfiguration.FindSlot(TEXT("Default")))
            return Default->Definition;
    return FOriginPivotDefinition();
}

bool FOriginPivotAuthoring::SaveSourceDefinition(UOriginPivotComponent* Pivot,
    const FOriginPivotDefinition& Definition, FText& Reason)
{
    if (!CanAuthor(Pivot))
    { Reason = LOCTEXT("SourceNotEditable", "This Origin Pivot cannot be edited in the current context."); return false; }
    if (Definition.Offset.ContainsNaN() || Definition.CustomPosition.ContainsNaN())
    { Reason = LOCTEXT("SourceInvalidNumbers", "Source coordinates must be finite."); return false; }
    FOriginPivotSlot* Default = Pivot->OriginPivotConfiguration.FindSlot(TEXT("Default"));
    if (!Default)
    { Reason = LOCTEXT("NoDefaultSource", "This pivot has no Default slot. Repair the pivot configuration before changing its source."); return false; }
    if (SameDefinition(Default->Definition, Definition))
    { Reason = FText::GetEmpty(); return true; }
    UBlueprint* BP = nullptr; USCS_Node* Node = nullptr;
    ResolveAuthored(Pivot, BP, Node);
    const FScopedTransaction Transaction(LOCTEXT("SaveSource", "Origin: Change Pivot Source Rule"));
    if (BP) BP->Modify();
    Pivot->Modify();
    Default->Definition = Definition; // Slot pose and active name remain untouched.
    NotifyChanged(Pivot, BP);
    Reason = FText::GetEmpty();
    return true;
}

void FOriginPivotAuthoring::ListDirectSourceComponents(UOriginPivotComponent* Pivot, TArray<FName>& OutNames)
{
    OutNames.Reset();
    if (!Pivot) return;
    if (Pivot->IsTemplate())
    {
        UBlueprint* BP = nullptr; USCS_Node* Node = nullptr;
        if (ResolveAuthored(Pivot, BP, Node))
            for (USCS_Node* ChildNode : Node->GetChildNodes())
                if (ChildNode && Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()))
                    OutNames.Add(ChildNode->GetVariableName());
    }
    else
    {
        TArray<USceneComponent*> Children;
        Pivot->GetChildrenComponents(false, Children);
        for (USceneComponent* Child : Children)
            if (IsValid(Child) && !Child->IsVisualizationComponent()) OutNames.Add(Child->GetFName());
    }
    OutNames.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

void FOriginPivotAuthoring::ListSourceSockets(UOriginPivotComponent* Pivot,
    FName ComponentName, TArray<FName>& OutNames)
{
    OutNames.Reset();
    OutNames.Add(NAME_None);
    if (USceneComponent* Source = DirectSource(Pivot, ComponentName))
        for (FName Name : Source->GetAllSocketNames()) OutNames.AddUnique(Name);
}

bool FOriginPivotAuthoring::ComputeSourceCandidate(UOriginPivotComponent* Pivot,
    const FOriginPivotDefinition& Definition, FTransform& OutCandidate, FText& Reason)
{
    if (!CanAuthor(Pivot))
    { Reason = LOCTEXT("SourceUnavailable", "Select an editable Origin Pivot component before recalculating."); return false; }
    const bool bAuthored = Pivot->IsTemplate();
    FOriginPivotCalculationInput Inputs;
    OutCandidate = bAuthored ? Pivot->GetRelativeTransform() : Pivot->GetComponentTransform();
    Inputs.PivotRotation = OutCandidate.GetRotation();
    if (bAuthored)
    {
        UBlueprint* BP = nullptr; USCS_Node* Node = nullptr;
        if (!ResolveAuthored(Pivot, BP, Node))
        { Reason = LOCTEXT("NoSCS", "The selected component is not an authored Blueprint SCS pivot."); return false; }
        // Blueprint template transforms are evaluated in the pivot parent's
        // authored frame; no preview Actor is used and no compile is triggered.
        for (USCS_Node* ChildNode : Node->GetChildNodes())
        {
            const USceneComponent* Child = ChildNode
                ? Cast<USceneComponent>(ChildNode->ComponentTemplate.Get()) : nullptr;
            if (!IsValid(Child) || Child->IsUsingAbsoluteLocation() ||
                Child->IsUsingAbsoluteRotation() || Child->IsUsingAbsoluteScale() ||
                !Child->GetAttachSocketName().IsNone())
            { Reason = LOCTEXT("UnsupportedAuthored", "An authored child uses an unsupported absolute transform or attachment socket."); return false; }
            const FTransform ChildInFrame = Child->GetRelativeTransform() * OutCandidate;
            if (!ChildInFrame.IsValid() || ChildInFrame.ContainsNaN())
            { Reason = LOCTEXT("InvalidAuthored", "An authored child has an invalid transform."); return false; }
            Inputs.DirectChildOrigins.Add(ChildInFrame.GetLocation());
        }
        for (USCS_Node* ChildNode : Node->GetChildNodes())
        {
            const USceneComponent* Child = Cast<USceneComponent>(ChildNode->ComponentTemplate.Get());
            const FTransform ChildInFrame = Child->GetRelativeTransform() * OutCandidate;
            FBox BranchGeometry(ForceInit);
            bool bUnsupported = false;
            if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child))
            {
                const FBox Bounds = Primitive->CalcBounds(ChildInFrame).GetBox();
                if (Bounds.IsValid) BranchGeometry += Bounds;
            }
            GatherAuthoringGeometry(ChildNode, ChildInFrame, BranchGeometry, bUnsupported);
            if (bUnsupported)
            { Reason = LOCTEXT("InvalidSCSBranch", "The authored component hierarchy has invalid transforms, sockets or absolute attachments."); return false; }
            Inputs.AssemblyBounds += BranchGeometry.IsValid
                ? BranchGeometry : FBox(ChildInFrame.GetLocation(), ChildInFrame.GetLocation());
        }
    }
    else
    {
        TArray<USceneComponent*> Direct;
        Pivot->GetChildrenComponents(false, Direct);
        for (USceneComponent* Child : Direct)
        {
            if (!IsValid(Child) || Child->IsVisualizationComponent()) continue;
            if (!Child->IsRegistered() || Child->GetComponentTransform().ContainsNaN())
            { Reason = LOCTEXT("UnregisteredSourceChild", "The source assembly has an unregistered or invalid component."); return false; }
            Inputs.DirectChildOrigins.Add(Child->GetComponentLocation());
            FBox BranchGeometry(ForceInit);
            bool bUnsupported = false;
            GatherLiveGeometry(Child, BranchGeometry, bUnsupported);
            if (bUnsupported)
            { Reason = LOCTEXT("InvalidSourceBranch", "The source assembly contains an unregistered or invalid descendant."); return false; }
            Inputs.AssemblyBounds += BranchGeometry.IsValid
                ? BranchGeometry : FBox(Child->GetComponentLocation(), Child->GetComponentLocation());
        }
    }

    if (Definition.PositionMode == EOriginPivotMode::ChildActor ||
        Definition.PositionMode == EOriginPivotMode::ComponentSocket)
    {
        USceneComponent* Child = DirectSource(Pivot, Definition.ComponentName);
        if (!Child || (Definition.ChildActor.IsValid() && Child->GetOwner() != Definition.ChildActor.Get()))
        { Reason = LOCTEXT("NoSourceComponent", "Choose a valid direct child component as the source. A missing component never moves the pivot."); return false; }
        if (bAuthored)
        {
            Inputs.ReferencePosition = (Child->GetRelativeTransform() * OutCandidate).GetLocation();
            if (Definition.PositionMode == EOriginPivotMode::ComponentSocket && !Definition.SocketName.IsNone())
            {
                if (!Child->DoesSocketExist(Definition.SocketName))
                { Reason = LOCTEXT("NoSourceSocket", "The source component socket does not exist."); return false; }
                const FTransform SourceInFrame = Child->GetRelativeTransform() * OutCandidate;
                Inputs.ReferencePosition = (Child->GetSocketTransform(Definition.SocketName, RTS_Component) *
                    SourceInFrame).GetLocation();
            }
        }
        else
        {
            if (!Child->IsRegistered() ||
                (!Definition.SocketName.IsNone() &&
                    !Child->DoesSocketExist(Definition.SocketName)))
            { Reason = LOCTEXT("InvalidSourceSocket", "The selected direct child is unregistered or the requested socket is missing."); return false; }
            Inputs.ReferencePosition = Definition.PositionMode == EOriginPivotMode::ComponentSocket &&
                !Definition.SocketName.IsNone()
                    ? Child->GetSocketLocation(Definition.SocketName) : Child->GetComponentLocation();
        }
        Inputs.bHasReferencePosition = true;
    }
    else if (Definition.PositionMode == EOriginPivotMode::ReferenceActor)
    {
        AActor* Reference = Definition.ReferenceActor.Get();
        if (bAuthored || !IsValid(Reference) || Reference->GetWorld() != Pivot->GetWorld() ||
            Reference == Pivot->GetOwner())
        { Reason = LOCTEXT("InvalidExternalReference", "Select a loaded reference Actor in the same level world. Blueprint templates cannot resolve world Actor references."); return false; }
        for (const USceneComponent* Parent = Reference->GetRootComponent(); Parent; Parent = Parent->GetAttachParent())
            if (Parent == Pivot)
            { Reason = LOCTEXT("ReferenceIsChild", "Reference Actors must be outside this pivot assembly. Select a direct child source instead."); return false; }
        Inputs.ReferencePosition = Reference->GetActorLocation();
        Inputs.bHasReferencePosition = true;
    }
    FVector Position = FVector::ZeroVector;
    FString Error;
    if (!FOriginPivotSnapMath::CalculatePosition(Definition, Inputs, Position, Error))
    { Reason = FText::FromString(Error); return false; }
    OutCandidate.SetTranslation(Position);
    Reason = FText::GetEmpty();
    return true;
}

bool FOriginPivotAuthoring::RecalculateSourcePivot(UOriginPivotComponent* Pivot, FText& Reason)
{
    const FOriginPivotDefinition Definition = ReadSourceDefinition(Pivot);
    FTransform Candidate;
    if (!ComputeSourceCandidate(Pivot, Definition, Candidate, Reason)) return false;
    return ApplyCandidate(Pivot, Candidate, NAME_None, false,
        LOCTEXT("RecalculateSource", "Origin: Recalculate Pivot From Source"), Reason);
}

#undef LOCTEXT_NAMESPACE
