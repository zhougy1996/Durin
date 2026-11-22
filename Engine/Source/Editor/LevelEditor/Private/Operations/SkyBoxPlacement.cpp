#include "Operations/SkyBoxPlacement.h"
#include "Transactions/Transactor.h"
#include "Actors/EditorActorSubsystem.h"
#include "Editor/EditorEngine.h"

#include "Actors/SkyBoxActor.h"
#include "Components/SkyBoxComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Texture/TextureCube.h"

namespace Durin::LevelEditor
{
	namespace
	{
		struct FSkyBoxCandidate
		{
			DSkyBoxComponent* Component = nullptr;
			AActor* Actor = nullptr;
		};

		auto FindVisibleSkyBoxes(DLevel& Level) -> std::vector<FSkyBoxCandidate>
		{
			std::vector<FSkyBoxCandidate> Candidates;
			for (const TObjectPtr<AActor>& ActorPtr : Level.GetActors())
			{
				AActor* Actor = ActorPtr.Get();
				if (!Actor || Actor->IsHidden()) continue;
				for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetComponents())
				{
					auto* Component = Cast<DSkyBoxComponent>(ComponentPtr.Get());
					if (Component) Candidates.push_back({Component, Actor});
				}
			}
			return Candidates;
		}
	}

	auto PlaceSkyBox(DEditorActorSubsystem& Editor, DLevel& Level, DTextureCube* Cube, FName Name,
		const FActorEditContext& Context) -> std::expected<AActor*, std::string>
	{
		if (!IsInGameThread()) return std::unexpected("Actor edits require the game thread.");
		if (!IsValid(Cube)) return std::unexpected("The dropped TextureCube is unavailable.");
		if (!IsValid(&Level)) return std::unexpected("The target level is unavailable.");

		const auto Candidates = FindVisibleSkyBoxes(Level);
		if (Candidates.size() > 1)
			return std::unexpected("Multiple visible sky boxes exist. Resolve the conflict before replacing the active sky box.");

		if (Candidates.empty())
		{
			auto Configure = [Cube](AActor& Actor) {
				if (!IsValid(Cube)) throw std::runtime_error("The dropped TextureCube retired during creation.");
				static_cast<ASkyBoxActor&>(Actor).GetSkyBoxComponent()->SetTextureCube(Cube);
			};
			std::string Error;
			auto* Actor = Editor.SpawnActor(Level, ASkyBoxActor::StaticClass(), Name, {}, Context, Configure,
				ESpawnActorNameMode::Requested, {}, &Error);
			if (!Actor) return std::unexpected(std::move(Error));
			return Actor;
		}

		auto [Component, Actor] = Candidates.front();
		if (!IsValid(Component) || !IsValid(Actor)) return std::unexpected("The active sky box is unavailable.");
		auto Admission = Editor.CanEditActor(*Actor, Context);
		if (!Admission) return std::unexpected(std::move(Admission.error()));
		if (Component->GetTextureCube() == Cube) return Actor;

		auto* Transactions = Editor.GetEditor()->GetTransactor();
		FScopedTransaction Recording("SkyBox", "Set sky box texture", Component);
		if (!Recording.IsOutstanding()) return std::unexpected("The editor could not begin the sky box texture transaction.");
		Component->SetFlags(EObjectFlags::Transactional);
		if (!Component->Modify(false) || !Transactions->IsActive())
		{
			return std::unexpected("The sky box component could not be captured for editing.");
		}
		Component->SetTextureCube(Cube);
		auto* Property = Component->GetClass()->FindPropertyByName("TextureCube");
		Component->PostEditChangeProperty({.MemberProperty = Property, .LeafProperty = Property});
		return Actor;
	}
}
