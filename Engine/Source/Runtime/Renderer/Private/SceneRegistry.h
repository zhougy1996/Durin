#pragma once

#include "Scene.h"

namespace Durin
{
	// Owns the authoritative render-thread light membership and its family indexes.
	class FLightSceneRegistry final
	{
	public:
		auto AddOrReplace(FScene& Scene,
			std::shared_ptr<FLightSceneProxy> Proxy) -> void;
		auto Remove(FLightSceneId SceneId, uint64 Revision) -> void;
		auto Clear() -> void;

		auto GetDirectional() const -> const std::vector<FLightSceneInfo*>&
		{
			return Directional;
		}
		auto GetPoint() const -> const std::vector<FLightSceneInfo*>&
		{
			return Point;
		}
		auto GetSpot() const -> const std::vector<FLightSceneInfo*>&
		{
			return Spot;
		}

	private:
		auto Attach(FLightSceneInfo& Info) -> void;
		auto Detach(FLightSceneInfo& Info) -> void;
		auto Accept(FLightSceneId SceneId, uint64 Revision) -> bool;

		std::unordered_map<FLightSceneId,
			std::unique_ptr<FLightSceneInfo>, FSceneIdHash> InfosById;
		std::unordered_map<FLightSceneId, uint64, FSceneIdHash> LastSeenRevisions;
		std::vector<FLightSceneInfo*> Directional;
		std::vector<FLightSceneInfo*> Point;
		std::vector<FLightSceneInfo*> Spot;
	};

	// Owns sky-box candidates and their deterministic active-candidate policy.
	class FSkyBoxSceneRegistry final
	{
	public:
		auto AddOrReplace(FScene& Scene,
			std::shared_ptr<FSkyBoxSceneProxy> Proxy) -> void;
		auto Remove(FSkyBoxSceneId SceneId, uint64 Revision) -> void;
		auto Clear() -> void;
		auto GetActive() const -> const FSkyBoxSceneInfo*;
		auto Num() const -> size_t { return SceneInfos.size(); }

	private:
		auto Accept(FSkyBoxSceneId SceneId, uint64 Revision) -> bool;

		std::unordered_map<FSkyBoxSceneId,
			std::unique_ptr<FSkyBoxSceneInfo>, FSceneIdHash> InfosById;
		std::unordered_map<FSkyBoxSceneId, uint64, FSceneIdHash> LastSeenRevisions;
		std::vector<FSkyBoxSceneInfo*> SceneInfos;
	};

	// Owns global cloud candidates and resolves the active eligible publication.
	class FVolumetricCloudSceneRegistry final
	{
	public:
		auto AddOrReplace(FScene& Scene,
			std::shared_ptr<FVolumetricCloudSceneProxy> Proxy) -> void;
		auto Remove(FVolumetricCloudSceneId SceneId, uint64 Revision) -> void;
		auto Clear() -> void;
		auto GetActive() const -> const FVolumetricCloudSceneInfo*;
		auto Num() const -> size_t { return SceneInfos.size(); }

	private:
		auto Accept(FVolumetricCloudSceneId SceneId, uint64 Revision) -> bool;

		std::unordered_map<FVolumetricCloudSceneId,
			std::unique_ptr<FVolumetricCloudSceneInfo>, FSceneIdHash> InfosById;
		std::unordered_map<FVolumetricCloudSceneId, uint64, FSceneIdHash>
			LastSeenRevisions;
		std::vector<FVolumetricCloudSceneInfo*> SceneInfos;
	};
}
