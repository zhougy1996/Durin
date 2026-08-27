#pragma once

#include "EngineAPI.h"
#include "IScene.h"

namespace Durin
{
	enum class ELightSceneProxyKind : uint8
	{
		Directional,
		Point,
		Spot,
	};

	// Owns a detached renderer-facing light value and exposes its explicit family.
	class FLightSceneProxy
	{
	public:
		ENGINE_API virtual ~FLightSceneProxy() = default;
		virtual auto GetKind() const -> ELightSceneProxyKind = 0;
	};

	class FDirectionalLightSceneProxy final : public FLightSceneProxy
	{
	public:
		explicit FDirectionalLightSceneProxy(FDirectionalLightSceneData InData)
			: Data(std::move(InData)) {}

		auto GetKind() const -> ELightSceneProxyKind override
		{
			return ELightSceneProxyKind::Directional;
		}
		auto GetData() const -> const FDirectionalLightSceneData& { return Data; }

	private:
		FDirectionalLightSceneData Data;
	};

	class FPointLightSceneProxy final : public FLightSceneProxy
	{
	public:
		explicit FPointLightSceneProxy(FPointLightSceneData InData)
			: Data(std::move(InData)) {}

		auto GetKind() const -> ELightSceneProxyKind override
		{
			return ELightSceneProxyKind::Point;
		}
		auto GetData() const -> const FPointLightSceneData& { return Data; }

	private:
		FPointLightSceneData Data;
	};

	class FSpotLightSceneProxy final : public FLightSceneProxy
	{
	public:
		explicit FSpotLightSceneProxy(FSpotLightSceneData InData)
			: Data(std::move(InData)) {}

		auto GetKind() const -> ELightSceneProxyKind override
		{
			return ELightSceneProxyKind::Spot;
		}
		auto GetData() const -> const FSpotLightSceneData& { return Data; }

	private:
		FSpotLightSceneData Data;
	};
}
