#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	struct FTerrainHeightmapBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 ProducerVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && ProducerVersion != 0;
		}
	};

	struct FTerrainHeightmapRecipeRequest
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		std::function<bool()> ShouldCancel;
	};

	struct FTerrainHeightmapRecipeProduct
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
	};

	class ITerrainHeightmapBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.TerrainHeightmapBuildProvider";
		static constexpr uint32 FeatureVersion = 1;

		virtual auto GetHeightmapDescriptor() const
			-> FTerrainHeightmapBuildProviderDescriptor = 0;
		virtual auto Build(
			FTerrainHeightmapRecipeRequest Request,
			FTerrainHeightmapRecipeProduct& OutProduct,
			std::string& OutError) -> bool = 0;
	};

}
