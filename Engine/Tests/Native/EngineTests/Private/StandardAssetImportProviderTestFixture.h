#pragma once

#include "StandardAssetAuthoringTestSupport.h"
#include "StandardAssetImportProviders.h"

#include <string>

namespace Durin::Tests
{
	class FScopedStandardAssetImportProviders
	{
	public:
		FScopedStandardAssetImportProviders() = default;
		FScopedStandardAssetImportProviders(const FScopedStandardAssetImportProviders&) = delete;
		auto operator=(const FScopedStandardAssetImportProviders&)
			-> FScopedStandardAssetImportProviders& = delete;

		~FScopedStandardAssetImportProviders()
		{
			if (bRegistered)
			{
				Asset::Import::Standard::UnregisterStandardAssetImportProviders();
			}
		}

		auto Register(std::string& OutError) -> bool
		{
			bRegistered = Asset::Import::Standard::RegisterStandardAssetImportProviders(
				OutError, GetEngineTestModuleCallbackGate());
			return bRegistered;
		}

	private:
		bool bRegistered = false;
	};
}
