#include "SkyBoxPlacement.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorChange.h"
#include "Actors/EditorActorSubsystem.h"

#include "Actors/SkyBoxActor.h"
#include "Components/SkyBoxComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
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

		// Creates and removes one actor while preserving its requested level identity.
		class FCreateSkyBoxTransaction final : public ::Durin::Editor::FActorCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return Presence ? Presence->GetModifiedObjects() : std::vector<DObject*>{}; }

			FCreateSkyBoxTransaction(DLevel* InLevel, DTextureCube* InTextureCube, FName InActorName, const std::string& Label)
				: Level(InLevel), TextureCube(InTextureCube), ActorName(InActorName)
			{
				AffectedPackages.front() = InLevel ? InLevel->GetPackage() : nullptr;
				if (!InLevel || !IsValid(InTextureCube)) return;
				FScopedPackageDirtySuppression SuppressPreparation;
				auto* Prepared = NewObject<ASkyBoxActor>(InLevel, ActorName);
				if (!Prepared || !Prepared->RerunConstructionScripts())
				{ if (Prepared) MarkObjectHierarchyAsGarbage(Prepared); return; }
				Prepared->SetInitialActorLabel(InLevel->MakeUniqueActorLabel(Label));
				Prepared->GetSkyBoxComponent()->SetTextureCube(InTextureCube);
				AActor* Base = Prepared;
				Presence = MakeActorPresenceChange(*InLevel, {&Base, 1}, false, "Place sky box");
				Prepared->SetTransient(true);
				Actor = Prepared;
			}
			~FCreateSkyBoxTransaction() override
			{
				if (!bEverPublished && IsValid(Actor.Get()) && Level && !Level->ContainsActor(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get());
			}

			auto GetDescription() const -> std::string_view override { return "Place sky box"; }
			auto GetAllocatedSize() const -> size_t override { return Presence ? Presence->GetAllocatedSize() : 0; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetDetails(::Durin::ETransactionOperation Operation) const -> std::string override
			{
				return Operation == ::Durin::ETransactionOperation::Undo
					? std::format("Remove sky box '{}'", ActorName)
					: std::format("Create sky box '{}'", ActorName);
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Replay(::Durin::ETransactionOperation Operation) -> ::Durin::Editor::FActorChangeResult override
			{
				using Code = ::Durin::Editor::EActorChangeError;
				const auto Reject = [&](Code Error) -> ::Durin::Editor::FActorChangeResult {
					return {{.Code = Error, .TargetPath = Level ? Level->GetObjectPath() : std::string{}, .TargetLabel = ActorName.ToString()}};
				};
				if (!Level || !Presence) return Reject(Code::TargetUnavailable);
				if (Operation != ::Durin::ETransactionOperation::Undo && !IsValid(TextureCube.Get())) return Reject(Code::ResourceUnavailable);
				auto Result = Presence->Replay(Operation);
				if (Result) bEverPublished = true;
				return Result;
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				if (Presence) Presence->AddReferencedObjects(Collector);
				for (DObject* Object : {static_cast<DObject*>(Level.Get()),
					static_cast<DObject*>(TextureCube.Get()),
					static_cast<DObject*>(Actor.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}

		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<DTextureCube> TextureCube;
			TObjectPtr<ASkyBoxActor> Actor;
			std::unique_ptr<::Durin::Editor::FActorCommandChange> Presence;
			bool bEverPublished = false;
			FName ActorName;
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
		auto Apply = [&](std::unique_ptr<FActorCommandChange> Transaction) -> FSkyBoxPlacementResult
		{
			if (Transactions)
			{
				auto Applied = ::Durin::Editor::ApplyEditorChange(*Transactions, std::move(Transaction));
				if (Applied) return {};
				auto Result = Reject(ESkyBoxPlacementError::Transaction);
				Result.Message = FormatTransactorResult(Applied);
				return Result;
			}
			auto Applied = Transaction->Replay(ETransactionOperation::Redo);
			if (Applied) return {};
			auto Result = Reject(ESkyBoxPlacementError::Replay);
			Result.Message = FormatActorChangeError(Applied.Error);
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

			FScopedTransaction Recording(Transactions, {"SkyBox", "Set sky box texture", Component});
			if (Transactions)
			{
				if (!Recording.IsOutstanding()) return Reject(ESkyBoxPlacementError::Transaction);
				const auto Record = RecordObjectForEdit(*Transactions, *Component);
				if (!Record) return Reject(ESkyBoxPlacementError::Transaction);
			}
			Component->SetTextureCube(TextureCube);
			auto* Property = Component->GetClass()->FindPropertyByName("TextureCube");
			Component->PostEditChangeProperty({.MemberProperty = Property, .LeafProperty = Property});
			(void)Recording.End();
			if (Transactions && (Transactions->GetLastResult().Code == ETransactorResultCode::Failed
				|| Transactions->GetLastResult().Code == ETransactorResultCode::RecoveryRequired)) return Reject(ESkyBoxPlacementError::Transaction);
			if (!Transactions && Level.GetPackage()) Level.GetPackage()->MarkDirty();
			return {.Actor = Actor, .bChanged = true};
		}

		const FName ActorName = Level.MakeUniqueActorName(RequestedName);
		auto Transaction = std::make_unique<FCreateSkyBoxTransaction>(&Level, TextureCube, ActorName, RequestedName.ToString());
		if (auto Applied = Apply(std::move(Transaction)); !Applied) return Applied;
		if (!Transactions && Level.GetPackage()) Level.GetPackage()->MarkDirty();
		return {
			.Actor = Level.FindActorByName(ActorName),
			.bChanged = true,
		};
	}
}
