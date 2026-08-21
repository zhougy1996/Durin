#pragma once

#include "AssetRuntimeServicesInternal.h"

namespace Durin::Asset
{
	// Owns AssetCore's private services and coordinates their shared lifecycle.
	class FAssetRuntimeState
	{
	public:
		ASSETCORE_API static auto Get() -> FAssetRuntimeState&;

		ASSETCORE_API auto Initialize(FAssetRuntimeConfiguration Configuration)
			-> FAssetResult;
		ASSETCORE_API auto StopAcceptingRequests() -> void;
		auto IsAcceptingRequests() const -> bool { return bAcceptingRequests; }
		ASSETCORE_API auto Shutdown() -> void;
		auto GetRuntimeConfiguration() const -> const FAssetRuntimeConfiguration&
		{
			return RuntimeConfiguration;
		}

		auto GetCatalogStore() -> FAssetCatalogStore& { return Catalog; }
		auto GetCatalogStore() const -> const FAssetCatalogStore& { return Catalog; }
		auto GetLoadService() -> FAssetLoadService& { return Loader; }
		auto GetLoadService() const -> const FAssetLoadService& { return Loader; }
		auto GetMutationCoordinator() -> FAssetMutationCoordinator& { return Mutations; }

	private:
		FAssetRuntimeState();

		FAssetRuntimeConfiguration RuntimeConfiguration =
			FAssetRuntimeConfiguration::Authored();
		bool bAcceptingRequests = true;
		FAssetCatalogStore Catalog;
		FAssetResidencyStore Residency;
		FAssetLoadService Loader;
		FAssetMutationCoordinator Mutations;
	};
}
