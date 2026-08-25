#include "TerrainLevelAuthoring.h"

#include "Actors/TerrainActor.h"
#include "Components/TerrainComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Engine/Level.h"
#include "Math/Operations.h"
#include "Terrain/TerrainHeightmap.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto MakeDiagnostic(ETerrainLevelAuthoringError Error, std::string Message)
			-> FTerrainLevelAuthoringDiagnostic
		{
			return {.Error = Error, .Message = std::move(Message)};
		}

		auto IsFiniteTransform(const FTransform& Transform) -> bool
		{
			return Math::IsFinite(Transform.Translation)
				&& Math::IsFinite(Transform.Rotation)
				&& Math::IsFinite(Transform.Scale3D);
		}

		auto ApplyPlacement(DLevel& Level, const FTerrainPlacementPlan& Plan,
			ATerrainActor*& OutActor, std::string& OutError) -> bool
		{
			OutActor = nullptr;
			DPackage* Package = Level.GetPackage();
			const bool bPackageWasDirty = Package && Package->IsDirty();
			auto RestoreDirtyState = [&]
			{
				if (Package && !bPackageWasDirty) Package->ClearDirty();
			};
			if (Level.FindActorByName(Plan.ActorName))
			{
				OutError = std::format("Actor name '{}' is occupied.", Plan.ActorName.ToString());
				return false;
			}
			auto* Actor = Level.SpawnActor<ATerrainActor>(Plan.ActorName);
			if (!Actor || Actor->GetFName() != Plan.ActorName)
			{
				if (Actor) Level.DestroyActor(Actor);
				RestoreDirtyState();
				OutError = "The Terrain actor could not be created.";
				return false;
			}
			DTerrainComponent* Component = Actor->GetTerrainComponent();
			Component->SetHeightmap(Plan.Heightmap.Get());
			if (!Component->SetSampleSpacing(Plan.SpacingX, Plan.SpacingY)
				|| !Component->SetHeightRange(Plan.HeightScale, Plan.HeightOffset)
				|| !Actor->SetActorTransform(Plan.Transform))
			{
				Level.DestroyActor(Actor);
				RestoreDirtyState();
				OutError = "The Terrain authored properties could not be applied.";
				return false;
			}
			Component->SetVisible(Plan.bVisible);
			OutActor = Actor;
			return true;
		}

		class FTerrainPlacementTransaction final : public ::Durin::Editor::ITransaction
		{
		public:
			explicit FTerrainPlacementTransaction(const FTerrainPlacementPlan& InPlan)
				: Plan(InPlan)
			{
				AffectedPackages.front() = Plan.Package.Get();
			}
			auto GetDescription() const -> std::string_view override { return Plan.Description; }
			auto GetDetails(::Durin::Editor::ETransactionOperation) const -> std::string override
			{
				return LastError.empty() ? std::format("Place Terrain '{}'", Plan.ActorName.ToString()) : LastError;
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Undo() -> bool override
			{
				LastError.clear();
				DLevel* Level = Plan.Level.Get();
				AActor* Actor = Level ? Level->FindActorByName(Plan.ActorName) : nullptr;
				auto* Terrain = Cast<ATerrainActor>(Actor);
				if (!Level || !Terrain)
				{
					LastError = "The placed Terrain no longer exists.";
					return false;
				}
				if (!Level->DestroyActor(Terrain))
				{
					LastError = "The placed Terrain could not be removed.";
					return false;
				}
				return true;
			}
			auto Redo() -> bool override
			{
				LastError.clear();
				ATerrainActor* Actor = nullptr;
				return Plan.Level && ApplyPlacement(*Plan.Level, Plan, Actor, LastError);
			}

		private:
			FTerrainPlacementPlan Plan;
			std::array<DPackage*, 1> AffectedPackages{};
			std::string LastError;
		};
	}

	auto FTerrainLevelAuthoringService::CaptureTarget(DLevel& Level) -> FTerrainPlacementRequest
	{
		DPackage* Package = Level.GetPackage();
		return {.Level = &Level,
			.ExpectedPackagePath = Package ? Package->GetPackagePath() : std::string(),
			.ExpectedPackageEditRevision = Package ? Package->GetEditRevision() : 0};
	}

	auto FTerrainLevelAuthoringService::Plan(const FTerrainPlacementRequest& Request)
		-> FTerrainPlacementPlan
	{
		FTerrainPlacementPlan Result;
		if (GIsGameThreadIdInitialized && !IsInGameThread())
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::WrongThread,
				"Terrain authoring must run on the game thread.");
			return Result;
		}
		if (!Request.Level || Request.ActorName.IsNone() || !Request.Heightmap)
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::InvalidRequest,
				"A Level, actor name, and Terrain heightmap are required.");
			return Result;
		}
		if (Request.bReadOnly)
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::ReadOnly,
				"The target Level is read-only.");
			return Result;
		}
		DPackage* Package = Request.Level->GetPackage();
		if (!Package || !Package->IsAssetPackage()
			|| Package->GetPackagePath() != Request.ExpectedPackagePath
			|| Package->GetEditRevision() != Request.ExpectedPackageEditRevision)
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::StaleTarget,
				"The target Level package no longer matches the captured request.");
			return Result;
		}
		if (Request.Level->FindActorByName(Request.ActorName))
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::NameConflict,
				std::format("Actor name '{}' is already occupied.", Request.ActorName.ToString()));
			return Result;
		}
		const uint64 HeightmapRevision = Request.Heightmap->GetRevision();
		if (!IsValid(Request.Heightmap.Get()) || Request.Heightmap->GetStatus() != ETerrainHeightmapStatus::Ready
			|| !Request.Heightmap->GetPayload()
			|| (Request.ExpectedHeightmapRevision != 0 && Request.ExpectedHeightmapRevision != HeightmapRevision))
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::UnavailableHeightmap,
				"The Terrain heightmap is unavailable or changed before planning.");
			return Result;
		}
		if (!IsFiniteTransform(Request.Transform)
			|| !std::isfinite(Request.SpacingX) || Request.SpacingX <= 0.0
			|| !std::isfinite(Request.SpacingY) || Request.SpacingY <= 0.0
			|| !std::isfinite(Request.HeightScale) || !std::isfinite(Request.HeightOffset))
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::InvalidProperties,
				"Terrain transform and height properties must be finite; spacing must be positive.");
			return Result;
		}
		Result.Level = Request.Level;
		Result.Package = Package;
		Result.PackagePath = Package->GetPackagePath();
		Result.PackageEditRevision = Package->GetEditRevision();
		Result.ActorHierarchyRevision = Request.Level->GetEditorActorHierarchyRevision();
		Result.ActorName = Request.ActorName;
		Result.Heightmap = Request.Heightmap;
		Result.HeightmapRevision = HeightmapRevision;
		Result.Transform = Request.Transform;
		Result.SpacingX = Request.SpacingX;
		Result.SpacingY = Request.SpacingY;
		Result.HeightScale = Request.HeightScale;
		Result.HeightOffset = Request.HeightOffset;
		Result.bVisible = Request.bVisible;
		Result.Description = Request.Description.empty() ? "Place terrain actor" : Request.Description;
		return Result;
	}

	auto FTerrainLevelAuthoringService::Execute(const FTerrainPlacementPlan& Plan,
		const FTerrainLevelExecutionContext& Context) -> FTerrainPlacementResult
	{
		FTerrainPlacementResult Result;
		if (!Plan) { Result.Diagnostic = Plan.Diagnostic; return Result; }
		DLevel* Level = Plan.Level.Get();
		DPackage* Package = Plan.Package.Get();
		if (Context.bReadOnly)
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::ReadOnly,
				"The target Level became read-only before execution.");
			return Result;
		}
		if (!Level || Context.OpenLevel != Level || !Package || Level->GetPackage() != Package
			|| Package->GetPackagePath() != Plan.PackagePath
			|| Package->GetEditRevision() != Plan.PackageEditRevision
			|| Level->GetEditorActorHierarchyRevision() != Plan.ActorHierarchyRevision
			|| !Plan.Heightmap || Plan.Heightmap->GetRevision() != Plan.HeightmapRevision
			|| Plan.Heightmap->GetStatus() != ETerrainHeightmapStatus::Ready)
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::StaleTarget,
				"The Level or heightmap changed after Terrain placement was planned.");
			return Result;
		}
		auto Transaction = std::make_unique<FTerrainPlacementTransaction>(Plan);
		const bool bSucceeded = Context.Transactions
			? Context.Transactions->Execute(std::move(Transaction)) : Transaction->Redo();
		if (!bSucceeded)
		{
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::ExecutionFailed,
				"Terrain placement could not be applied.");
			return Result;
		}
		Result.Actor = Cast<ATerrainActor>(Level->FindActorByName(Plan.ActorName));
		Result.bChanged = Result.Actor != nullptr;
		if (!Result.bChanged)
			Result.Diagnostic = MakeDiagnostic(ETerrainLevelAuthoringError::ExecutionFailed,
				"Terrain placement completed without a live actor.");
		return Result;
	}
}
