#pragma once

#include "AssetTools/IAssetTools.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	// Owns the editor asset service for one manager-controlled module generation.
	class FAssetToolsModule final : public IModuleInterface
	{
	public:
		ASSETTOOLS_API auto StartupModule() -> void override;
		ASSETTOOLS_API auto ShutdownModule() -> void override;
		ASSETTOOLS_API auto Get() -> IAssetTools&;

		static auto GetModule() -> FAssetToolsModule&
		{
			return FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		}

	private:
		std::unique_ptr<IAssetTools> AssetTools;
	};
}
