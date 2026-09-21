#include "SkyBoxPlacement.h"

#include "Actors/SkyBoxActor.h"
#include "Components/SkyBoxComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Transactions/Transactor.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Texture/TextureCube.h"

namespace Durin::Editor::Level
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
		class FCreateSkyBoxTransaction final : public ::Durin::Editor::ITransactionCustomChange
		{
		public:
			FCreateSkyBoxTransaction(DLevel* InLevel, DTextureCube* InTextureCube, FName InActorName)
				: Level(InLevel), TextureCube(InTextureCube), ActorName(InActorName)
			{
				AffectedPackages.front() = InLevel ? InLevel->GetPackage() : nullptr;
			}

			auto GetDescription() const -> std::string_view override { return "Place sky box"; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetDetails(::Durin::Editor::ETransactionOperation Operation) const -> std::string override
			{
				return Operation == ::Durin::Editor::ETransactionOperation::Undo
					? std::format("Remove sky box '{}'", ActorName)
					: std::format("Create sky box '{}'", ActorName);
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Replay(::Durin::Editor::ETransactionOperation Operation) -> ::Durin::Editor::FTransactionCustomResult override
			{
				using Code = ::Durin::Editor::ETransactionCustomError;
				const auto Reject = [&](Code Error) -> ::Durin::Editor::FTransactionCustomResult {
					return {{.Code = Error, .TargetPath = Level ? Level->GetObjectPath() : std::string{}, .TargetLabel = ActorName.ToString()}};
				};
				if (!Level) return Reject(Code::TargetUnavailable);
				if (Operation == ::Durin::Editor::ETransactionOperation::Undo)
				{
					ASkyBoxActor* Existing = Actor.Get();
					if (!Existing || !Level->ContainsActor(Existing)) return Reject(Code::ActorMembership);
					if (!Level->DestroyActor(Existing)) return Reject(Code::ActorDestroy);
					Actor = nullptr;
					return {};
				}
				if (!IsValid(TextureCube.Get())) return Reject(Code::ResourceUnavailable);
				if (Level->FindActorByName(ActorName)) return Reject(Code::ActorNameCollision);
				auto* Created = Level->SpawnActor<ASkyBoxActor>(ActorName);
				if (!Created) return Reject(Code::ActorSpawn);
				Created->GetSkyBoxComponent()->SetTextureCube(TextureCube.Get());
				Actor = Created;
				return {};
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {static_cast<DObject*>(Level.Get()),
					static_cast<DObject*>(TextureCube.Get()),
					static_cast<DObject*>(Actor.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}

		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<DTextureCube> TextureCube;
			TObjectPtr<ASkyBoxActor> Actor;
			FName ActorName;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		// Restores the cube reference on one existing skybox component.
		class FSetSkyBoxTextureTransaction final : public ::Durin::Editor::ITransactionCustomChange
		{
		public:
			FSetSkyBoxTextureTransaction(DSkyBoxComponent* InComponent, DTextureCube* InBefore, DTextureCube* InAfter)
				: Component(InComponent), Before(InBefore), After(InAfter)
			{
				AffectedPackages.front() = InComponent ? InComponent->GetPackage() : nullptr;
			}

			auto GetDescription() const -> std::string_view override { return "Set sky box texture"; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Replay(::Durin::Editor::ETransactionOperation Operation) -> ::Durin::Editor::FTransactionCustomResult override
			{ return Apply(Operation == ::Durin::Editor::ETransactionOperation::Undo ? Before.Get() : After.Get()); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {static_cast<DObject*>(Component.Get()),
					static_cast<DObject*>(Before.Get()),
					static_cast<DObject*>(After.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}

		private:
			auto Apply(DTextureCube* TextureCube) -> ::Durin::Editor::FTransactionCustomResult
			{
				if (!Component) return {{.Code = ::Durin::Editor::ETransactionCustomError::TargetUnavailable}};
				if (TextureCube && !IsValid(TextureCube)) return {{.Code = ::Durin::Editor::ETransactionCustomError::ResourceUnavailable,
					.TargetPath = Component->GetObjectPath()}};
				Component->SetTextureCube(TextureCube);
				return {};
			}

			TObjectPtr<DSkyBoxComponent> Component;
			TObjectPtr<DTextureCube> Before;
			TObjectPtr<DTextureCube> After;
			std::array<DPackage*, 1> AffectedPackages{};
		};
	}

	static auto FormatSkyBoxPlacementError(ESkyBoxPlacementError Error) -> std::string
	{
		switch (Error)
		{
		case ESkyBoxPlacementError::None: return {};
		case ESkyBoxPlacementError::ReadOnly: return "The level is read-only.";
		case ESkyBoxPlacementError::TextureUnavailable: return "The dropped TextureCube is unavailable.";
		case ESkyBoxPlacementError::MultipleSkyBoxes: return "Multiple visible sky boxes exist. Resolve the conflict before replacing the active sky box.";
		case ESkyBoxPlacementError::SkyBoxUnavailable: return "The active sky box is unavailable.";
		case ESkyBoxPlacementError::Transaction: return "Sky box transaction failed.";
		case ESkyBoxPlacementError::Replay: return "Sky box replay failed.";
		}
		return {};
	}

	auto FSkyBoxPlacement::PlaceTextureCube(
		DLevel& Level,
		DTextureCube* TextureCube,
		FName RequestedName,
		::Durin::DTransactor* Transactions,
		bool bReadOnly) -> FSkyBoxPlacementResult
	{
		auto Reject = [](ESkyBoxPlacementError Code) -> FSkyBoxPlacementResult
		{
			return {.Error = Code, .Message = FormatSkyBoxPlacementError(Code)};
		};
		auto Apply = [&](std::unique_ptr<ITransactionCustomChange> Transaction) -> FSkyBoxPlacementResult
		{
			if (Transactions)
			{
				auto Applied = Transactions->Execute(std::move(Transaction));
				if (Applied) return {};
				auto Result = Reject(ESkyBoxPlacementError::Transaction);
				Result.Message = FormatTransactorResult(Applied);
				return Result;
			}
			auto Applied = Transaction->Replay(ETransactionOperation::Redo);
			if (Applied) return {};
			auto Result = Reject(ESkyBoxPlacementError::Replay);
			Result.Message = FormatTransactionCustomError(Applied.Error);
			return Result;
		};
		if (bReadOnly) return Reject(ESkyBoxPlacementError::ReadOnly);
		if (!IsValid(TextureCube)) return Reject(ESkyBoxPlacementError::TextureUnavailable);

		const std::vector<FSkyBoxCandidate> Candidates = FindVisibleSkyBoxes(Level);
		if (Candidates.size() > 1)
			return Reject(ESkyBoxPlacementError::MultipleSkyBoxes);

		if (!Candidates.empty())
		{
			DSkyBoxComponent* Component = Candidates.front().Component;
			AActor* Actor = Candidates.front().Actor;
			if (!Component || !Actor) return Reject(ESkyBoxPlacementError::SkyBoxUnavailable);
			if (Component->GetTextureCube() == TextureCube) return {.Actor = Actor};

			auto Transaction = std::make_unique<FSetSkyBoxTextureTransaction>(
				Component, Component->GetTextureCube(), TextureCube);
			if (auto Applied = Apply(std::move(Transaction)); !Applied) return Applied;
			if (!Transactions && Level.GetPackage()) Level.GetPackage()->MarkDirty();
			return {.Actor = Actor, .bChanged = true};
		}

		const FName ActorName = MakeUniqueActorName(Level, RequestedName);
		auto Transaction = std::make_unique<FCreateSkyBoxTransaction>(&Level, TextureCube, ActorName);
		if (auto Applied = Apply(std::move(Transaction)); !Applied) return Applied;
		if (!Transactions && Level.GetPackage()) Level.GetPackage()->MarkDirty();
		return {
			.Actor = Level.FindActorByName(ActorName),
			.bChanged = true,
		};
	}
}
