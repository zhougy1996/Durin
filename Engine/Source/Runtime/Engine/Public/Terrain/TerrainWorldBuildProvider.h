#pragma once

#include "Terrain/TerrainWorld.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	struct FTerrainWorldBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 BuilderVersion = 0;
		uint16 ProductSchemaVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && BuilderVersion != 0
				&& ProductSchemaVersion != 0;
		}
	};

	struct FTerrainWorldRecipeRequest
	{
		FTerrainTileRecipeInput Input;
	};

	struct FTerrainWorldRecipeProduct
	{
		std::array<FByteArray, 5> Bodies;
	};

	class ITerrainWorldBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.TerrainWorldBuildProvider";
		static constexpr uint32 FeatureVersion = 1;

		virtual auto GetTerrainWorldDescriptor() const
			-> FTerrainWorldBuildProviderDescriptor = 0;
		virtual auto Build(FTerrainWorldRecipeRequest Request,
			FTerrainWorldRecipeProduct& OutProduct,
			ETerrainWorldOutcome& OutOutcome,
			std::string& OutError) -> bool = 0;
	};

}
