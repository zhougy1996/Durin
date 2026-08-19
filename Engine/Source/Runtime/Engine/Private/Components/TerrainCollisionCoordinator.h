#pragma once

#include "Components/TerrainComponent.h"
#include "DObject/ObjectHandle.h"
#include "Threading/Task.h"

namespace Durin
{
	// Owns Terrain collision generation, caching, diagnostics, and game-thread publication.
	class FTerrainCollisionCoordinator final
	{
	public:
		explicit FTerrainCollisionCoordinator(DTerrainComponent& InOwner);
		~FTerrainCollisionCoordinator();

		auto OnRegistered() -> void;
		auto OnUnregistered() -> void;
		auto OnCollisionSettingsChanged() -> void;
		auto OnSourceChanged() -> void;
		auto PrepareForSourceRevisionChange() -> void;
		auto GetStatus() const -> ETerrainCollisionStatus;
		auto GetDiagnostic() const -> const std::string&;
		auto GetRevision() const -> uint64 { return CollisionRevision; }
		auto GetFacts() const -> FTerrainCollisionFacts;
		auto RequestPhysicsStateCreation(bool bWaitUntilReady) -> bool;
		auto BuildCollisionGeometry(
			FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool;

	private:
		struct FBuildState
		{
			FObjectHandle ComponentHandle;
			FObjectHandle WorldHandle;
			uint64 RegistrationGeneration = 0;
			uint64 AssetRevision = 0;
			uint64 CollisionRevision = 0;
			std::shared_ptr<const FTerrainHeightmapPayload> Payload;
			double SpacingX = 0.0;
			double SpacingY = 0.0;
			double HeightScale = 0.0;
			double HeightOffset = 0.0;
			FTaskCancellationSource Cancellation;
			FTaskHandle Worker;
			FTaskHandle Publisher;
			std::mutex ResultMutex;
			FCollisionGeometryRef Geometry;
			FCollisionGeometryBuildDiagnostics Diagnostics;
			bool bWorkerCompleted = false;
			bool bCanceled = false;
		};

		auto CancelBuild() -> void;
		auto PublishCompletedBuild(const std::shared_ptr<FBuildState>& Build) -> bool;
		auto SetFailure(ETerrainCollisionStatus Status, std::string Diagnostic) const -> bool;
		auto ResetCache() const -> void;
		auto SetStatus(ETerrainCollisionStatus Status, std::string Diagnostic = {}) const -> void;
		auto CacheMatches(
			const std::shared_ptr<const FTerrainHeightmapPayload>& Payload,
			uint64 AssetRevision) const -> bool;

		DTerrainComponent& Owner;
		uint64 CollisionRevision = 1;
		mutable uint64 CachedHeightmapRevision = 0;
		mutable std::shared_ptr<const FTerrainHeightmapPayload> CachedPayload;
		mutable FCollisionGeometryRef CachedGeometry;
		mutable FCollisionGeometryBuildDiagnostics CachedDiagnostics;
		mutable double CachedSpacingX = 0.0;
		mutable double CachedSpacingY = 0.0;
		mutable double CachedHeightScale = 0.0;
		mutable double CachedHeightOffset = 0.0;
		mutable uint64 LastPhysicsInsertionNanoseconds = 0;
		std::shared_ptr<FBuildState> ActiveBuild;
	};
} // namespace Durin
