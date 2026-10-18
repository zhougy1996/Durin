#include "Tools/EditorScriptCall.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/EditorToolService.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Components/ActorComponent.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
    namespace
    {
        thread_local FScopedEditorScriptCall* GScriptCall = nullptr;
        struct FPolicy
        {
            FObjectKey Host, Level;
            std::function<EEditorScriptTargetAccess()> Resolve;
            bool Active = true;
        };
        auto Policies() -> std::map<uint64, std::shared_ptr<FPolicy>>&
        { static std::map<uint64, std::shared_ptr<FPolicy>> Values; return Values; }
        uint64 NextPolicy = 1;
    }
    FScopedEditorScriptCall::FScopedEditorScriptCall(FEditorScriptCallContext InContext)
        : Context(std::move(InContext)), Previous(GScriptCall)
    { CheckGameThread(); GScriptCall = this; }
    FScopedEditorScriptCall::~FScopedEditorScriptCall() { GScriptCall = Previous; }
    auto FScopedEditorScriptCall::Current() -> FScopedEditorScriptCall*
    { return IsInGameThread() ? GScriptCall : nullptr; }
    auto FScopedEditorScriptCall::IsAdmitted() const -> bool
    {
        if (!IsInGameThread() || !IsValid(Context.Host) || Context.Host->AreEditorSubsystemsUpdating()
            || Context.Host->GetEditorSubsystemState() != ESubsystemState::Ready || !Context.Gate || !Context.Gate->IsOpen()
            || Context.Cancellation.IsCancellationRequested() || Context.Client.Cancellation.IsCancellationRequested()) return false;
        auto* Tools = Context.Host->GetEditorSubsystem<DEditorToolSubsystem>();
        return Tools && Tools->GetService()->GetWorkGate() == Context.Gate;
    }
    auto FScopedEditorScriptCall::ResolveEditAccess(DObject& Target) -> EEditorScriptTargetAccess
    {
        if (!IsAdmitted() || Context.Host->IsPlaying() || !IsValid(&Target)) return EEditorScriptTargetAccess::Unavailable;
        DLevel* Level = Cast<DLevel>(&Target);
        auto* Actor = Cast<AActor>(&Target);
        if (auto* Component = Cast<DActorComponent>(&Target))
        {
            Actor = Component->GetOwner();
            if (!IsValid(Actor) || !Actor->OwnsComponent(Component)) return EEditorScriptTargetAccess::Unavailable;
        }
        if (Actor)
        {
            Level = Cast<DLevel>(Actor->GetOuter());
            if (!IsValid(Level) || !Level->ContainsActor(Actor)) return EEditorScriptTargetAccess::Unavailable;
        }
        auto* World = Context.Host->GetEditorWorld();
        if (!IsValid(Level) || !IsValid(World) || Level->GetWorld() != World || World->GetCurrentLevel() != Level)
            return EEditorScriptTargetAccess::Unavailable;
        std::shared_ptr<FPolicy> Match;
        for (const auto& [Id, Policy] : Policies())
            if (Policy->Host == FObjectKey(Context.Host) && Policy->Level == FObjectKey(Level))
            { if (Match) return EEditorScriptTargetAccess::Unavailable; Match = Policy; }
        if (Match)
        {
            const auto Result = Match->Resolve();
            return Match->Active && IsAdmitted() && Context.Host->GetEditorWorld() == World && World->GetCurrentLevel() == Level
                && IsValid(Level) && !Context.Host->IsPlaying() ? Result : EEditorScriptTargetAccess::Unavailable;
        }
        return Context.EditPolicy == EEditorScriptEditPolicy::NonDocumentReadWrite
            ? EEditorScriptTargetAccess::Writable : Context.EditPolicy == EEditorScriptEditPolicy::NonDocumentReadOnly
            ? EEditorScriptTargetAccess::ReadOnly : EEditorScriptTargetAccess::Unavailable;
    }
    FEditorScriptTargetPolicyRegistration::FEditorScriptTargetPolicyRegistration(DEditorEngine& Host, DLevel& Level,
        std::function<EEditorScriptTargetAccess()> Resolve) : Identity(NextPolicy++)
    {
        CheckGameThread(); require(Resolve);
        Policies().emplace(Identity, std::make_shared<FPolicy>(FPolicy{FObjectKey(&Host), FObjectKey(&Level), std::move(Resolve)}));
    }
    FEditorScriptTargetPolicyRegistration::~FEditorScriptTargetPolicyRegistration()
    { CheckGameThread(); auto It = Policies().find(Identity); if (It != Policies().end()) { It->second->Active = false; Policies().erase(It); } }
}
