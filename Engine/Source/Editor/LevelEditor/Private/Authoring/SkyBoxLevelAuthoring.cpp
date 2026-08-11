#include "SkyBoxLevelAuthoring.h"

#include "Actors/SkyBoxActor.h"
#include "Components/SkyBoxComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Texture/TextureCube.h"

namespace Durin
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
				for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
				{
					auto* Component = Cast<DSkyBoxComponent>(ComponentPtr.Get());
					if (Component) Candidates.push_back({Component, Actor});
				}
			}
			std::ranges::sort(Candidates, [](const FSkyBoxCandidate& A, const FSkyBoxCandidate& B) {
				return std::tuple(A.Component->GetSkyBoxSceneId(), A.Component->GetObjectPath(),
					A.Component->GetSkyBoxInstanceId())
					< std::tuple(B.Component->GetSkyBoxSceneId(), B.Component->GetObjectPath(),
						B.Component->GetSkyBoxInstanceId());
			});
			return Candidates;
		}

		auto MakeUniqueActorName(DLevel& Level, FName Requested) -> FName
		{
			if (!Level.FindActorByName(Requested)) return Requested;
			const std::string Base = Requested.ToString();
			for (uint32 Suffix = 2;; ++Suffix)
			{
				FName Candidate(std::format("{}_{}", Base, Suffix));
				if (!Level.FindActorByName(Candidate)) return Candidate;
			}
		}

		// Creates and removes one actor while preserving its requested level identity.
		class FCreateSkyBoxTransaction final : public Editor::ITransaction
		{
		public:
			FCreateSkyBoxTransaction(DLevel* InLevel, DTextureCube* InTextureCube, FName InActorName)
				: Level(InLevel), TextureCube(InTextureCube), ActorName(InActorName)
			{
				AffectedPackages.front() = InLevel ? InLevel->GetPackage() : nullptr;
			}

			auto GetDescription() const -> std::string_view override { return "Place sky box"; }
			auto GetDetails(Editor::ETransactionOperation Operation) const -> std::string override
			{
				return Operation == Editor::ETransactionOperation::Undo
					? std::format("Remove sky box '{}'", ActorName.ToString())
					: std::format("Create sky box '{}'", ActorName.ToString());
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Undo() -> bool override
			{
				ASkyBoxActor* Existing = Actor.Get();
				if (!Level || !Existing || !Level->ContainsActor(Existing)) return false;
				if (!Level->DestroyActor(Existing)) return false;
				Actor = nullptr;
				return true;
			}
			auto Redo() -> bool override
			{
				if (!Level || !IsValid(TextureCube.Get()) || Level->FindActorByName(ActorName)) return false;
				auto* Created = Level->SpawnActor<ASkyBoxActor>(ActorName);
				if (!Created) return false;
				Created->GetSkyBoxComponent()->SetTextureCube(TextureCube.Get());
				Actor = Created;
				return true;
			}

		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<DTextureCube> TextureCube;
			TObjectPtr<ASkyBoxActor> Actor;
			FName ActorName;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		// Restores the cube reference on one existing skybox component.
		class FSetSkyBoxTextureTransaction final : public Editor::ITransaction
		{
		public:
			FSetSkyBoxTextureTransaction(DSkyBoxComponent* InComponent, DTextureCube* InBefore, DTextureCube* InAfter)
				: Component(InComponent), Before(InBefore), After(InAfter)
			{
				AffectedPackages.front() = InComponent ? InComponent->GetPackage() : nullptr;
			}

			auto GetDescription() const -> std::string_view override { return "Set sky box texture"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Undo() -> bool override { return Apply(Before.Get()); }
			auto Redo() -> bool override { return Apply(After.Get()); }

		private:
			auto Apply(DTextureCube* TextureCube) -> bool
			{
				if (!Component || (TextureCube && !IsValid(TextureCube))) return false;
				Component->SetTextureCube(TextureCube);
				return true;
			}

			TObjectPtr<DSkyBoxComponent> Component;
			TObjectPtr<DTextureCube> Before;
			TObjectPtr<DTextureCube> After;
			std::array<DPackage*, 1> AffectedPackages{};
		};
	}

	auto FSkyBoxLevelAuthoringService::PlaceTextureCube(
		DLevel& Level,
		DTextureCube* TextureCube,
		FName RequestedName,
		Editor::FTransactionManager* Transactions,
		bool bReadOnly) -> FSkyBoxPlacementResult
	{
		if (bReadOnly) return {.Message = "The level is read-only."};
		if (!IsValid(TextureCube)) return {.Message = "The dropped TextureCube is unavailable."};

		const std::vector<FSkyBoxCandidate> Candidates = FindVisibleSkyBoxes(Level);
		if (Candidates.size() > 1)
			return {.Message = "Multiple visible sky boxes exist. Resolve the conflict before replacing the active sky box."};

		if (!Candidates.empty())
		{
			DSkyBoxComponent* Component = Candidates.front().Component;
			AActor* Actor = Candidates.front().Actor;
			if (!Component || !Actor) return {.Message = "The active sky box is unavailable."};
			if (Component->GetTextureCube() == TextureCube) return {.Actor = Actor};

			auto Transaction = std::make_unique<FSetSkyBoxTextureTransaction>(
				Component, Component->GetTextureCube(), TextureCube);
			const bool bApplied = Transactions
				? Transactions->Execute(std::move(Transaction))
				: Transaction->Redo();
			if (!bApplied) return {.Message = "Failed to replace the active sky box texture."};
			if (!Transactions && Level.GetPackage()) Level.GetPackage()->MarkDirty();
			return {.Actor = Actor, .bChanged = true};
		}

		const FName ActorName = MakeUniqueActorName(Level, RequestedName);
		auto Transaction = std::make_unique<FCreateSkyBoxTransaction>(&Level, TextureCube, ActorName);
		const bool bApplied = Transactions
			? Transactions->Execute(std::move(Transaction))
			: Transaction->Redo();
		if (!bApplied) return {.Message = "Failed to create a sky box actor."};
		if (!Transactions && Level.GetPackage()) Level.GetPackage()->MarkDirty();
		return {
			.Actor = Level.FindActorByName(ActorName),
			.bChanged = true,
		};
	}
}
