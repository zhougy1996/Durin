#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DTextureCube;
	class ITextureCubePostLoadFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.TextureCubePostLoad";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto PostLoadUncooked(DTextureCube& Texture, std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeTextureCubeUncookedPostLoadHandler(
		DTextureCube& Texture, std::string& OutError) -> bool;
}
