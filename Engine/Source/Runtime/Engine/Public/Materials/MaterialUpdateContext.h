#pragma once

#include "DObject/ObjectHandle.h"
#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

namespace Durin
{
	class DMaterialInterface;

	// Reports the deterministic work performed by one material update flush.
	struct FMaterialUpdateCounters
	{
		uint64 RootCount = 0;
		uint64 ObjectSnapshotCount = 0;
		uint64 ScannedObjectCount = 0;
		uint64 TestedMaterialCount = 0;
		uint64 AffectedMaterialCount = 0;
		uint64 ScannedComponentCount = 0;
		uint64 UpdatedSlotCount = 0;
	};

	// Batches material roots and invalidates loaded dependents from one stable object snapshot.
	class FMaterialUpdateContext
	{
	public:
		ENGINE_API FMaterialUpdateContext();
		ENGINE_API ~FMaterialUpdateContext();

		FMaterialUpdateContext(const FMaterialUpdateContext&) = delete;
		auto operator=(const FMaterialUpdateContext&) -> FMaterialUpdateContext& = delete;

		// Adds one live root idempotently and merges flags with any existing entry.
		ENGINE_API auto AddMaterial(
			DMaterialInterface* Material,
			EMaterialRenderDirtyFlags DirtyFlags
		) -> void;

		// Computes the loaded closure from one snapshot and emits current per-slot updates synchronously.
		ENGINE_API auto Flush() -> void;

		auto GetCounters() const -> const FMaterialUpdateCounters& { return Counters; }

	private:
		struct FChangedRoot
		{
			FObjectHandle Handle;
			EMaterialRenderDirtyFlags DirtyFlags = EMaterialRenderDirtyFlags::None;
		};

		std::vector<FChangedRoot> ChangedRoots;
		FMaterialUpdateCounters Counters;
	};
}
