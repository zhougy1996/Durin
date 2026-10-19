#include "Actors/EditorActorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorScriptCall.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
    using namespace Editor;
    namespace
    {
        auto RawActors(const std::vector<TObjectPtr<AActor>>& Actors) -> std::vector<AActor*>
        {
            std::vector<AActor*> Raw;
            Raw.reserve(Actors.size());
            for (const auto& Actor : Actors) Raw.push_back(Actor.Get());
            return Raw;
        }
        auto RejectScriptCall(std::string Message) -> void
        {
            if (auto* Call = FScopedEditorScriptCall::Current()) Call->Reject(std::move(Message));
        }
    }

    auto DEditorActorSubsystem::ScriptGetEditorLevel() const -> DLevel*
    {
        auto* Call = FScopedEditorScriptCall::Current();
        auto* Host = GetEditor();
        if (!Call || !Call->IsAdmitted() || Call->GetContext().Host != Host
            || !IsValid(this) || !GetWorkGate() || !GetWorkGate()->IsOpen()
            || Host->GetEditorSubsystem<DEditorActorSubsystem>() != this)
        { RejectScriptCall("Actor query requires the admitting editor Host and live service."); return nullptr; }
        auto* World = Host->GetEditorWorld();
        if (!World) return nullptr;
        if (!IsValid(World) || World->IsShutdownRequested())
        { RejectScriptCall("The editor World is unavailable."); return nullptr; }
        auto* Level = World->GetCurrentLevel();
        if (Level && (!IsValid(Level) || Level->GetWorld() != World || Level->IsTemplateObject()))
        { RejectScriptCall("The current editor Level is unavailable."); return nullptr; }
        return Level;
    }
    auto DEditorActorSubsystem::ScriptGetAllLevelActors(DLevel* Level) const -> std::vector<TObjectPtr<AActor>>
    {
        auto* Current = ScriptGetEditorLevel();
        if (!IsValid(Level) || Level != Current)
        { RejectScriptCall("Actor query requires the current editor Level."); return {}; }
        std::vector<TObjectPtr<AActor>> Actors;
        for (const auto& Actor : Level->GetActors())
            if (IsValid(Actor.Get()) && !Actor->IsBeingDestroyed() && Actor->GetOuter() == Level)
                Actors.push_back(Actor);
        return Actors;
    }
    auto DEditorActorSubsystem::ScriptGetActorInfo(DLevel* Level, AActor* Actor) const -> FActorScriptInfo
    {
        const auto Actors = ScriptGetAllLevelActors(Level);
        if (!IsValid(Actor) || std::ranges::none_of(Actors, [Actor](const auto& Item) { return Item.Get() == Actor; }))
        { RejectScriptCall("Actor query requires a live published Actor in the current editor Level."); return {}; }
        FActorScriptInfo Info;
        Info.Actor = Actor;
        Info.Name = Actor->GetName();
        Info.ActorClass = Actor->GetClass();
        Info.Parent = Actor->GetAttachParentActor();
        for (const auto& Child : Actors)
            if (Child->GetAttachParentActor() == Actor) Info.Children.push_back(Child);
        Info.RootComponent = Actor->GetRootComponent();
        Info.WorldTransform = Actor->GetActorTransform();
        return Info;
    }
    auto DEditorActorSubsystem::ScriptEditContext(DLevel* Level) const -> std::optional<FActorEditContext>
    {
        auto* Call = FScopedEditorScriptCall::Current();
        if (!Call || !Call->IsAdmitted() || Call->GetContext().Host != GetEditor())
            return std::nullopt;
        if (!IsValid(Level))
            return std::nullopt;
        // Native Admit validates membership/play state, and re-resolves this live policy
        // after preparation callbacks. Never accept a script-supplied read-only bool.
        return FActorEditContext{Call->ResolveEditAccess(*Level) == EEditorScriptTargetAccess::ReadOnly};
    }
    auto DEditorActorSubsystem::ScriptSpawnActor(DLevel* Level, DClass* ActorClass, const FTransform& Transform, const std::string& Name) -> AActor*
    {
        const auto Context = ScriptEditContext(Level);
        if (!Context) return nullptr;
        return SpawnActor(*Level, ActorClass, FName(Name), Transform, *Context);
    }
    auto DEditorActorSubsystem::ScriptDestroyActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors) -> bool
    {
        const auto Context = ScriptEditContext(Level);
        if (!Context) return false;
        return DestroyActors(*Level, RawActors(Actors), *Context);
    }
    auto DEditorActorSubsystem::ScriptDuplicateActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors) -> std::vector<TObjectPtr<AActor>>
    {
        const auto Context = ScriptEditContext(Level);
        if (!Context) return {};
        return DuplicateActors(*Level, RawActors(Actors), *Context);
    }
    auto DEditorActorSubsystem::ScriptRenameActor(DLevel* Level, AActor* Actor, const std::string& Name) -> bool
    {
        const auto Context = ScriptEditContext(Level);
        if (!Context) return false;
        return RenameActor(*Level, Actor, FName(Name), *Context);
    }
    auto DEditorActorSubsystem::ScriptAttachActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors, AActor* Parent, EAttachmentTransformRule Rule) -> bool
    {
        const auto Context = ScriptEditContext(Level);
        if (!Context) return false;
        return AttachActors(*Level, RawActors(Actors), Parent, Rule, *Context);
    }
    auto DEditorActorSubsystem::ScriptDetachActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors, EDetachmentTransformRule Rule) -> bool
    {
        const auto Context = ScriptEditContext(Level);
        if (!Context) return false;
        return DetachActors(*Level, RawActors(Actors), Rule, *Context);
    }
}
