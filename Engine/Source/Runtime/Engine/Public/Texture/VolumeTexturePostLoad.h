#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DVolumeTexture;

	// Supplies uncooked DDC lookup and rebuild policy without an Engine-to-builder dependency.
	class IVolumeTextureAuthoringFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.VolumeTextureAuthoring";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto PostLoadUncooked(
			DVolumeTexture& Texture, std::string& OutError) -> bool = 0;
	};

	class IVolumeTextureInterchangeRecoveryFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.VolumeTextureInterchangeRecovery";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto RecoverUncooked(
			DVolumeTexture& Texture, std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeVolumeTextureUncookedPostLoadHandler(
		DVolumeTexture& Texture, std::string& OutError) -> bool;
	ENGINE_API auto TryInvokeVolumeTextureInterchangeRecovery(
		DVolumeTexture& Texture, std::string& OutError) -> std::optional<bool>;
}
