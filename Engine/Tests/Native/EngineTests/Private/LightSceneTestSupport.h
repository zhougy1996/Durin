#pragma once

#include "Rendering/LightSceneProxy.h"
#include "SceneTestAccess.h"

template<typename TProxy, typename TData>
auto PublishLightForTest(
	Durin::FScene& Scene,
	Durin::FLightSceneId Id,
	TData Data) -> TProxy*
{
	auto Proxy = std::make_unique<TProxy>(Id, std::move(Data));
	TProxy* Token = Proxy.get();
	return Durin::FSceneInterfaceTestAccess::TryAddLightProxy(Scene, std::move(Proxy)) ? Token : nullptr;
}
