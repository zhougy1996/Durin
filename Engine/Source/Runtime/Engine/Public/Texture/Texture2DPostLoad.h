#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DTexture2D;

	class ITexture2DAuthoringFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.Texture2DAuthoring";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto PostLoadUncooked(DTexture2D& Texture, std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeTexture2DUncookedPostLoadHandler(
		DTexture2D& Texture, std::string& OutError) -> bool;
}
