#pragma once

#include "CoreMinimal.h"
#include "Asset/PackageSerialization.h"

namespace Durin
{
	class FObjectGraphReplacement;
	// Only the owning graph replacement may publish a private candidate. Registry
	// rejection rolls back its files. Call inside the replacement's TryCommit callback.
	ENGINE_API auto SavePreparedPackage(DPackage* Package, const FObjectGraphReplacement& Publication,
		const FAssetPackageSaveArgs& Args = {}) -> FAssetWriteResult;

	// Stage before preparing replacement; no replacement snapshot spans disk I/O.
	class FPreparedAssetSave final
	{
	public:
		ENGINE_API static auto Begin(DPackage* Package, const FAssetPackageSaveArgs& Args,
			FAssetWriteResult& Admission) -> std::shared_ptr<FPreparedAssetSave>;
		ENGINE_API ~FPreparedAssetSave();
		ENGINE_API auto IsReady() const -> bool;
		ENGINE_API auto Commit(const FObjectGraphReplacement& Publication) -> FAssetWriteResult;
	private:
		FPreparedAssetSave();
		struct FState;
		std::unique_ptr<FState> State;
	};
}
