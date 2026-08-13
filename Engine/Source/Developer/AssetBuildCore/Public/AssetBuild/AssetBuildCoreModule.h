#pragma once

#include "Modules/ModuleManager.h"

namespace Durin::AssetBuild
{
	// Dynamic host-control surface for tools that must not import AssetBuildCore
	// on package-only process paths.
	class IAssetBuildCoreModule : public IModuleInterface
	{
	public:
		virtual auto InitializeHost(std::string* OutError = nullptr) -> bool = 0;
		virtual auto ShutdownHost() -> void = 0;
	};
}
