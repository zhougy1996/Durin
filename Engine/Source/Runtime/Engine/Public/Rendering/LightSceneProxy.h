#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Captures the renderer-facing directional-light state without retaining a component.
	struct FDirectionalLightSceneData
	{
		FVector3 Direction{-0.5, -0.5, -1.0};
		FVector3f Color{1.0f, 1.0f, 1.0f};
		float Intensity = 0.0f;
		float AmbientIntensity = 0.0f;
		float RimLightIntensity = 0.0f;
		bool bCastShadows = true;
	};

	// Captures renderer-facing point-light state in world space.
	struct FPointLightSceneData
	{
		FVector3 Position{0.0};
		FVector3f Color{1.0f};
		float Intensity = 0.0f;
		float Range = 1.0f;
	};

	// Captures renderer-facing spot-light state in world space and degrees.
	struct FSpotLightSceneData
	{
		FVector3 Position{0.0};
		FVector3 Direction{1.0, 0.0, 0.0};
		FVector3f Color{1.0f};
		float Intensity = 0.0f;
		float Range = 1.0f;
		float InnerConeAngle = 0.0f;
		float OuterConeAngle = 45.0f;
	};

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
