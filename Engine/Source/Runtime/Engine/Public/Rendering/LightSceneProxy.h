#pragma once

#include "EngineAPI.h"
#include "SceneTypes.h"

namespace Durin
{
	class FLightSceneInfo;

	// Carries stable light identity across the complete proxy-construction boundary.
	struct FLightSceneProxyDesc
	{
		FLightSceneId Id = InvalidLightSceneId;

		auto IsValid() const -> bool { return Id != InvalidLightSceneId; }
	};

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
		explicit FLightSceneProxy(FLightSceneProxyDesc InDesc)
			: Desc(std::move(InDesc)) {}
		ENGINE_API virtual ~FLightSceneProxy() = default;
		auto GetDesc() const -> const FLightSceneProxyDesc& { return Desc; }
		virtual auto GetKind() const -> ELightSceneProxyKind = 0;

	private:
		auto AttachToSceneInfo(FLightSceneInfo* InSceneInfo) -> void
		{
			check(SceneInfo == nullptr && InSceneInfo != nullptr);
			SceneInfo = InSceneInfo;
		}
		auto DetachFromSceneInfo(FLightSceneInfo* InSceneInfo) -> void
		{
			check(SceneInfo == InSceneInfo);
			SceneInfo = nullptr;
		}

		FLightSceneProxyDesc Desc;
		FLightSceneInfo* SceneInfo = nullptr;

		friend class FLightSceneInfo;
	};

	class FDirectionalLightSceneProxy final : public FLightSceneProxy
	{
	public:
		FDirectionalLightSceneProxy(FLightSceneProxyDesc InDesc,
			FDirectionalLightSceneData InData)
			: FLightSceneProxy(std::move(InDesc)), Data(std::move(InData)) {}
		FDirectionalLightSceneProxy(FLightSceneId Id,
			FDirectionalLightSceneData InData)
			: FDirectionalLightSceneProxy(
				FLightSceneProxyDesc{Id}, std::move(InData)) {}

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
		FPointLightSceneProxy(FLightSceneProxyDesc InDesc,
			FPointLightSceneData InData)
			: FLightSceneProxy(std::move(InDesc)), Data(std::move(InData)) {}
		FPointLightSceneProxy(FLightSceneId Id, FPointLightSceneData InData)
			: FPointLightSceneProxy(
				FLightSceneProxyDesc{Id}, std::move(InData)) {}

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
		FSpotLightSceneProxy(FLightSceneProxyDesc InDesc,
			FSpotLightSceneData InData)
			: FLightSceneProxy(std::move(InDesc)), Data(std::move(InData)) {}
		FSpotLightSceneProxy(FLightSceneId Id, FSpotLightSceneData InData)
			: FSpotLightSceneProxy(
				FLightSceneProxyDesc{Id}, std::move(InData)) {}

		auto GetKind() const -> ELightSceneProxyKind override
		{
			return ELightSceneProxyKind::Spot;
		}
		auto GetData() const -> const FSpotLightSceneData& { return Data; }

	private:
		FSpotLightSceneData Data;
	};
}
