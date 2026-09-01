#pragma once

#include "Scene.h"
#include "SceneInfo.h"

namespace Durin
{
	// Owns the authoritative render-thread light membership and its family indexes.
	class FLightSceneRegistry final
	{
	public:
		auto Add(FScene& Scene,
			std::shared_ptr<FLightSceneProxy> Proxy) -> void;
		auto Remove(FLightSceneProxy* Proxy) -> void;
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
		std::unordered_map<FLightSceneProxy*,
			std::unique_ptr<FLightSceneInfo>> InfosByProxy;
		std::vector<FLightSceneInfo*> Directional;
		std::vector<FLightSceneInfo*> Point;
		std::vector<FLightSceneInfo*> Spot;
	};

	// Owns sky-box candidates and their deterministic active-candidate policy.
	class FSkyBoxSceneRegistry final
	{
	public:
		auto Add(FScene& Scene,
			std::shared_ptr<FSkyBoxSceneProxy> Proxy) -> void;
		auto Remove(FSkyBoxSceneProxy* Proxy) -> void;
		auto Clear() -> void;
		auto GetActive() const -> const FSkyBoxSceneInfo*;
		auto Num() const -> size_t { return SceneInfos.size(); }

	private:
		std::unordered_map<FSkyBoxSceneProxy*,
			std::unique_ptr<FSkyBoxSceneInfo>> InfosByProxy;
		std::vector<FSkyBoxSceneInfo*> SceneInfos;
	};

	// Owns global cloud candidates and resolves the active eligible publication.
	class FVolumetricCloudSceneRegistry final
	{
	public:
		auto Add(FScene& Scene,
			std::shared_ptr<FVolumetricCloudSceneProxy> Proxy) -> void;
		auto Remove(FVolumetricCloudSceneProxy* Proxy) -> void;
		auto Clear() -> void;
		auto GetActive() const -> const FVolumetricCloudSceneInfo*;
		auto Num() const -> size_t { return SceneInfos.size(); }

	private:
		std::unordered_map<FVolumetricCloudSceneProxy*,
			std::unique_ptr<FVolumetricCloudSceneInfo>> InfosByProxy;
		std::vector<FVolumetricCloudSceneInfo*> SceneInfos;
	};
}
