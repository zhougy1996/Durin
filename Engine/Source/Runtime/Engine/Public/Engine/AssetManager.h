#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "Engine/AssetManagerTypes.h"
#include "Engine/StreamableManager.h"
#include "AssetManager.gen.h"

namespace Durin
{
	struct FPrimaryAssetManagerState;
	DCLASS(NoClassDefaultObject)
	class DAssetManager : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DAssetManager(const FObjectInitializer& Initializer);
		ENGINE_API ~DAssetManager() override;
		ENGINE_API virtual auto StartInitialLoading(FAssetManagerSettings Settings = {}) -> std::expected<void, std::string>;
		ENGINE_API auto Shutdown() -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto GetStreamableManager() -> FStreamableManager&;
		ENGINE_API auto GetPrimaryAssetIdList(FPrimaryAssetType Type) -> std::vector<FPrimaryAssetId>;
		ENGINE_API auto GetPrimaryAssetPath(FPrimaryAssetId Id) -> FSoftObjectPath;
		ENGINE_API auto GetPrimaryAssetIdForPath(const FTopLevelAssetPath& Path) -> FPrimaryAssetId;
		ENGINE_API auto GetPrimaryAssetRules(FPrimaryAssetId Id) -> FPrimaryAssetRules;
		ENGINE_API auto GetPrimaryAssetObject(FPrimaryAssetId Id) -> DObject*;
		ENGINE_API auto GetPrimaryAssetBundleState(FPrimaryAssetId Id, bool bPending = false) -> std::vector<FName>;
		ENGINE_API auto GetDiagnostics() -> std::vector<std::string>;
		ENGINE_API auto LoadPrimaryAsset(FPrimaryAssetId Id, std::span<const FName> Bundles = {}, FAssetManagerLoadParams Params = {}) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto LoadPrimaryAssets(std::span<const FPrimaryAssetId> Ids, std::span<const FName> Bundles = {}, FAssetManagerLoadParams Params = {}) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto ChangeBundleStateForPrimaryAssets(std::span<const FPrimaryAssetId> Ids,
			std::span<const FName> AddBundles, std::span<const FName> RemoveBundles,
			bool bRemoveAllBundles = false, FAssetManagerLoadParams Params = {}) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto UnloadPrimaryAsset(FPrimaryAssetId Id) -> bool;
		ENGINE_API auto UnloadPrimaryAssets(std::span<const FPrimaryAssetId> Ids) -> size_t;
	private:
		auto RefreshCatalog() -> void;
		auto Redirect(FPrimaryAssetId Id) const -> FPrimaryAssetId;
		auto Transition(std::span<const FPrimaryAssetId> Ids, std::span<const FName> Add,
			std::span<const FName> Remove, bool bRemoveAll, FAssetManagerLoadParams Params) -> std::shared_ptr<FStreamableHandle>;
		std::shared_ptr<FPrimaryAssetManagerState> State;
	};
}
