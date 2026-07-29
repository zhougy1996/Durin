#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"

namespace Durin
{
	class PrimitiveSceneProxy;
	class DDirectionalLightComponent;
	using FPrimitiveSceneId = uint64;
	inline constexpr FPrimitiveSceneId InvalidPrimitiveSceneId = 0;

	// Captures the renderer-facing directional light state without retaining a component.
	struct FDirectionalLightSceneData
	{
		FVector3 Direction{-0.5, -0.5, -1.0};
		FVector3f Color{1.0f, 1.0f, 1.0f};
		// A scene without an explicit light component must remain unlit. Light
		// components populate these values when they register with the scene.
		float Intensity = 0.0f;
		float AmbientIntensity = 0.0f;
		// Optional view-facing edge light used by editor preview scenes.
		float RimLightIntensity = 0.0f;
	};

	// Captures sky state without retaining or reading reflected objects on the render thread.
	struct FSkyBoxSceneData
	{
		// Persistent ordering key followed by a path tie-break for duplicated content.
		FGuid SceneId;
		std::string SelectionKey;

		// Runtime identity keeps duplicated components as distinct scene entries.
		uint64 InstanceId = 0;
		FRHITextureReferenceRef TextureReference;
		FQuat Rotation{1.0, 0.0, 0.0, 0.0};
		FVector3f Tint{1.0f, 1.0f, 1.0f};
		float Intensity = 1.0f;
		uint64 Revision = 0;
	};

	// Defines the game-thread mutation boundary of a renderer-owned scene.
	class IScene
	{
	public:
		ENGINE_API IScene() = default;
		ENGINE_API virtual ~IScene() = default;

		virtual auto AddOrReplacePrimitive(
			FPrimitiveSceneId PrimitiveId,
			std::unique_ptr<PrimitiveSceneProxy> Proxy,
			const FMatrix& Transform
		) -> void = 0;

		virtual auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void = 0;

		virtual auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void = 0;

		virtual auto UpdatePrimitiveMaterialBinding(
			FPrimitiveSceneId PrimitiveId,
			const FMaterialRenderProxyBindingUpdate& Update) -> void = 0;

		virtual auto Release() -> void = 0;

		virtual auto AddDirectionalLight(DDirectionalLightComponent* Light) -> void = 0;
		virtual auto RemoveDirectionalLight(DDirectionalLightComponent* Light) -> void = 0;
		virtual auto GetDirectionalLight(FDirectionalLightSceneData& OutLight) const -> bool = 0;

		virtual auto AddOrReplaceSkyBox(FSkyBoxSceneData Data) -> void = 0;
		virtual auto RemoveSkyBox(uint64 InstanceId, uint64 Revision) -> void = 0;
		virtual auto GetActiveSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool = 0;
		virtual auto GetSkyBoxCount_RenderThread() const -> size_t = 0;
	};
}
