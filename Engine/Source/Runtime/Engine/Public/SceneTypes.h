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

	// Identifies one immutable proxy publication within a typed scene registry.
	template<typename TSceneId>
	struct TSceneProxyMetadata
	{
		TSceneId SceneId;
		uint64 Revision = 0;

		auto IsValid() const -> bool
		{
			return SceneId.Value != 0 && Revision != 0;
		}
	};

	// Provides deterministic ordering for global scene candidates independently
	// of runtime registration order.
	struct FSceneCandidateIdentity
	{
		FGuid PersistentId;
		std::string SelectionKey;

		auto IsValid() const -> bool { return PersistentId.IsValid(); }
	};

	// Base for renderer-facing proxies whose identity is assigned exactly once
	// when they cross the scene publication boundary.
	template<typename TSceneId>
	class TSceneProxyPublication
	{
	public:
		auto GetMetadata() const -> const TSceneProxyMetadata<TSceneId>&
		{
			return Metadata;
		}

	private:
		auto BindPublication(TSceneProxyMetadata<TSceneId> InMetadata) -> bool
		{
			if (Metadata.IsValid() || !InMetadata.IsValid()) return false;
			Metadata = InMetadata;
			return true;
		}

		TSceneProxyMetadata<TSceneId> Metadata;

		friend class FScene;
	};
}
