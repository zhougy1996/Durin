#pragma once

#include "AssetForgeBuiltinsAssetTestSupport.h"
#include "AssetForgeBuiltinsProviders.h"
#include "EngineTestSupport.h"

#include <string>

namespace Durin::Tests
{
	class FScopedAssetForgeBuiltinsProviders
	{
	public:
		FScopedAssetForgeBuiltinsProviders() = default;
		FScopedAssetForgeBuiltinsProviders(const FScopedAssetForgeBuiltinsProviders&) = delete;
		auto operator=(const FScopedAssetForgeBuiltinsProviders&)
			-> FScopedAssetForgeBuiltinsProviders& = delete;

		~FScopedAssetForgeBuiltinsProviders()
		{
			if (bRegistered)
			{
				AssetForge::Builtins::UnregisterAssetForgeBuiltinsProviders();
			}
		}

		auto Register(std::string& OutError) -> bool
		{
			bRegistered = AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
				OutError, GetEngineTestModuleCallbackGate());
			return bRegistered;
		}

	private:
		bool bRegistered = false;
	};
}
