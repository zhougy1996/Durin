#pragma once

#include "AssetForgeAuthoringTestSupport.h"
#include "AssetForgeProviders.h"

#include <string>

namespace Durin::Tests
{
	class FScopedAssetForgeProviders
	{
	public:
		FScopedAssetForgeProviders() = default;
		FScopedAssetForgeProviders(const FScopedAssetForgeProviders&) = delete;
		auto operator=(const FScopedAssetForgeProviders&)
			-> FScopedAssetForgeProviders& = delete;

		~FScopedAssetForgeProviders()
		{
			if (bRegistered)
			{
				Asset::Forge::UnregisterAssetForgeProviders();
			}
		}

		auto Register(std::string& OutError) -> bool
		{
			bRegistered = Asset::Forge::RegisterAssetForgeProviders(
				OutError, GetEngineTestModuleCallbackGate());
			return bRegistered;
		}

	private:
		bool bRegistered = false;
	};
}
