#pragma once

#include "AssetRuntimeServicesInternal.h"

namespace Durin::Asset
{
	// Owns the Engine Asset subsystem's private services and shared lifecycle.
	class FAssetRuntimeState
	{
	public:
		ENGINE_API static auto Get() -> FAssetRuntimeState&;

		ENGINE_API auto Initialize(FAssetRuntimeConfiguration Configuration)
			-> FAssetResult;
		ENGINE_API auto StopAcceptingRequests() -> void;
		auto IsAcceptingRequests() const -> bool { return bAcceptingRequests; }
		ENGINE_API auto Shutdown() -> void;
		auto GetRuntimeConfiguration() const -> const FAssetRuntimeConfiguration&
		{
			return RuntimeConfiguration;
		}

		auto GetLoadService() -> FAssetLoadService& { return Loader; }
		auto GetLoadService() const -> const FAssetLoadService& { return Loader; }
		auto GetMutationCoordinator() -> FAssetMutationCoordinator& { return Mutations; }

	private:
		FAssetRuntimeState();

		FAssetRuntimeConfiguration RuntimeConfiguration =
			FAssetRuntimeConfiguration::Authored();
		bool bAcceptingRequests = true;
		FAssetResidencyStore Residency;
		FAssetLoadService Loader;
		FAssetMutationCoordinator Mutations;
	};
}
