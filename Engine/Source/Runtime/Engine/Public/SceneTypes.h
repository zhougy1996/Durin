#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Identifies one runtime entry within a typed renderer-scene collection.
	template<typename TTag>
	struct TSceneId
	{
		uint64 Value = 0;
		explicit constexpr TSceneId(uint64 InValue = 0) : Value(InValue) {}
		auto operator<=>(const TSceneId&) const = default;
	};

	struct FPrimitiveSceneIdTag;
	struct FLightSceneIdTag;
	struct FSkyBoxSceneIdTag;
	struct FVolumetricCloudSceneIdTag;
	using FPrimitiveSceneId = TSceneId<FPrimitiveSceneIdTag>;
	using FLightSceneId = TSceneId<FLightSceneIdTag>;
	using FSkyBoxSceneId = TSceneId<FSkyBoxSceneIdTag>;
	using FVolumetricCloudSceneId = TSceneId<FVolumetricCloudSceneIdTag>;
	inline constexpr FPrimitiveSceneId InvalidPrimitiveSceneId;
	inline constexpr FLightSceneId InvalidLightSceneId;
	inline constexpr FSkyBoxSceneId InvalidSkyBoxSceneId;
	inline constexpr FVolumetricCloudSceneId InvalidVolumetricCloudSceneId;

	// Hashes every typed scene identity by its runtime value.
	struct FSceneIdHash
	{
		template<typename TTag>
		auto operator()(TSceneId<TTag> Id) const -> size_t
		{
			return std::hash<uint64>{}(Id.Value);
		}
	};

}
