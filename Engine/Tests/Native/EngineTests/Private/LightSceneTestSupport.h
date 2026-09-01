#pragma once

#include "Rendering/LightSceneProxy.h"
#include "Scene.h"

template<typename TProxy, typename TData>
auto PublishLightForTest(
	Durin::FScene& Scene,
	Durin::FLightSceneId Id,
	TData Data) -> TProxy*
{
	auto Proxy = std::make_unique<TProxy>(Id, std::move(Data));
	TProxy* Token = Proxy.get();
	return Scene.AddLight(std::move(Proxy)) ? Token : nullptr;
}
