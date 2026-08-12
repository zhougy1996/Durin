#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class DTextureCube;
	using FTextureCubeUncookedPostLoadHandler =
		std::function<bool(DTextureCube&, std::string&)>;

	ENGINE_API auto RegisterTextureCubeUncookedPostLoadHandler(
		FTextureCubeUncookedPostLoadHandler Handler) -> bool;
	ENGINE_API auto UnregisterTextureCubeUncookedPostLoadHandler() -> void;
	ENGINE_API auto InvokeTextureCubeUncookedPostLoadHandler(
		DTextureCube& Texture, std::string& OutError) -> bool;
}
