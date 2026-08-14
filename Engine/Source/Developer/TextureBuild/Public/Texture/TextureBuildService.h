#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DAuthoringCoordinator.h"

namespace Durin::Asset::Build
{
	// Registers Texture coordination as one family-neutral authoring-host contribution.
	TEXTUREBUILD_API auto InitializeTextureBuildService(
		FModuleOwnedCallbackGate OwnerGate,
		const FTexture2DBuildCoordinatorConfig& Config = {}) -> bool;
	TEXTUREBUILD_API auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*;
	TEXTUREBUILD_API auto ShutdownTextureBuildService() -> void;
}
