#pragma once

#include "SceneInfo.h"
#include "Rendering/SkyBoxSceneProxy.h"

namespace Durin
{
	// Owns the authoritative render-thread light membership and its family indexes.
	class FLightSceneRegistry final
	{
	public:
		auto Add(std::shared_ptr<FLightSceneProxy> Proxy) -> void;
		auto Remove(FLightSceneProxy* Proxy) -> void;
		auto Clear() -> void;
		auto Num() const -> size_t { return InfosByProxy.size(); }

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
		std::unordered_map<FLightSceneProxy*, std::unique_ptr<FLightSceneInfo>> InfosByProxy;
		std::vector<FLightSceneInfo*> Directional;
		std::vector<FLightSceneInfo*> Point;
		std::vector<FLightSceneInfo*> Spot;
	};

	// Owns the scene's sole sky-box publication.
	class FSkyBoxSceneRegistry final
	{
	public:
		auto Add(std::shared_ptr<FSkyBoxSceneProxy> InProxy) -> void;
		auto Remove(FSkyBoxSceneProxy* Proxy) -> void;
		auto Clear() -> void;
		auto Get() const -> const FSkyBoxSceneProxy* { return Proxy.get(); }
		auto Num() const -> size_t { return Proxy ? 1 : 0; }

	private:
		std::shared_ptr<FSkyBoxSceneProxy> Proxy;
	};

	// Owns global cloud candidates and resolves the active eligible publication.
	class FVolumetricCloudSceneRegistry final
	{
	public:
		auto Add(std::shared_ptr<FVolumetricCloudSceneProxy> Proxy) -> void;
		auto Remove(FVolumetricCloudSceneProxy* Proxy) -> void;
		auto Clear() -> void;
		auto GetActive() const -> const FVolumetricCloudSceneInfo*;
		auto Num() const -> size_t { return SceneInfos.size(); }

	private:
		std::unordered_map<FVolumetricCloudSceneProxy*, std::unique_ptr<FVolumetricCloudSceneInfo>> InfosByProxy;
		std::vector<FVolumetricCloudSceneInfo*> SceneInfos;
	};
} // namespace Durin
