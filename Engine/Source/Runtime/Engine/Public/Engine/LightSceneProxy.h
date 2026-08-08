#pragma once

#include "EngineAPI.h"
#include "IScene.h"

namespace Durin
{
	class FLightSceneProxy
	{
	public:
		ENGINE_API virtual ~FLightSceneProxy() = default;
	};

	class FDirectionalLightSceneProxy final : public FLightSceneProxy
	{
	public:
		explicit FDirectionalLightSceneProxy(FDirectionalLightSceneData InData)
			: Data(std::move(InData)) {}

		auto GetData() const -> const FDirectionalLightSceneData& { return Data; }

	private:
		FDirectionalLightSceneData Data;
	};
}
