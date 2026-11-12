#pragma once

#include "CoreMinimal.h"
#include "Asset/BulkData.h"
#include "Asset/CookedMeshLoading.h"
#include "Asset/CookedMeshProducts.h"
#include "Threading/Task.h"

namespace Durin
{
	class DStaticMesh;
	class FCookedMeshLoadManager;

	// One asset's CPU load. Only Decode runs on a worker; lifecycle and publication
	// stay on GameThread. The mesh and scheduler retain this object until completion.
	class ENGINE_API FStaticMeshLoadUpdate
	{
	public:
		FStaticMeshLoadUpdate(DStaticMesh& InMesh, bool bInInitializeResources);
		~FStaticMeshLoadUpdate();
		auto Tick() -> void;
		auto Abort() -> void;
		auto Finish() -> void;
		auto IsCompleted() const -> bool { return Phase == EPhase::Completed; }
		auto IsCancelled() const -> bool { return Cancellation.IsCanceled(); }
		auto IsSuccessfullyFinished() const -> bool { return IsCompleted() && Error.Code == ECookedMeshLoadError::None; }

	private:
		friend class DStaticMesh;
		friend class FCookedMeshLoadManager;
		enum class EPhase : uint8 { Pending, Reading, Working, Ready, Completed };
		auto Start(FTaskAttribution InAttribution) -> void;
		auto Decode(FByteView RenderBytes, FByteView CollisionBytes) -> void;
		auto Complete() -> void;

		DStaticMesh* Mesh;
		uint64 ResourceRevision;
		uint64 MetadataIdentity;
		bool bInitializeResources;
		FBulkData RenderBulk;
		FBulkData CollisionBulk;
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		EBodySetupCollisionSourceMode CollisionMode;
		ECookTargetPlatform TargetPlatform;
		uint64 EstimatedBytes = 0;
		uint64 RetainedBytes = 0;
		EPhase Phase = EPhase::Pending;
		std::vector<FPackageResourceRequest> Reads;
		Tasks::FCancellationToken Cancellation;
		Tasks::FTask Task;
		FTaskAttribution Attribution;
		FStaticMeshCookedProduct Product;
		FCookedMeshLoadError Error;
	};
}
