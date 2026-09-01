#pragma once

#include "EngineAPI.h"
#include "SceneTypes.h"

namespace Durin
{
	class FPrimitiveSceneProxy;
	class FLightSceneProxy;
	class FSkyBoxSceneProxy;
	class FVolumetricCloudSceneProxy;
	struct FMaterialRenderProxyBindingUpdate;
	struct FSkeletalPosePalette;
	struct FSplineMeshRenderDynamicData;

	// Defines the game-thread publication boundary of a renderer-owned scene.
	class FSceneInterface
	{
	public:
		ENGINE_API FSceneInterface() = default;
		ENGINE_API virtual ~FSceneInterface() = default;

		virtual auto AddOrReplacePrimitive(
			FPrimitiveSceneId PrimitiveId,
			std::unique_ptr<FPrimitiveSceneProxy> Proxy,
			const FMatrix& Transform,
			bool bVisible = true
		) -> void = 0;

		virtual auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void = 0;

		virtual auto UpdatePrimitiveTransform(
			FPrimitiveSceneId PrimitiveId,
			const FMatrix& Transform) -> void = 0;
		virtual auto UpdatePrimitiveVisibility(
			FPrimitiveSceneId PrimitiveId,
			bool bVisible) -> void = 0;

		virtual auto UpdatePrimitiveMaterialBinding(
			FPrimitiveSceneId PrimitiveId,
			const FMaterialRenderProxyBindingUpdate& Update) -> void = 0;
		virtual auto UpdateSkeletalMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			std::shared_ptr<const FSkeletalPosePalette> Pose) -> void = 0;
		virtual auto UpdateSplineMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			FSplineMeshRenderDynamicData DynamicData) -> void = 0;

		// Add consumes Proxy in every case and returns true only when the render
		// command executed immediately or was admitted to the FIFO queue.
		virtual auto AddLight(std::unique_ptr<FLightSceneProxy> Proxy) -> bool = 0;
		virtual auto RemoveLight(FLightSceneProxy* Proxy) -> void = 0;

		// Components retain Proxy.get() only as an opaque removal token after a
		// successful Add; they must never dereference a published proxy.
		virtual auto AddSkyBox(std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> bool = 0;
		virtual auto RemoveSkyBox(FSkyBoxSceneProxy* Proxy) -> void = 0;

		virtual auto AddVolumetricCloud(
			std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> bool = 0;
		virtual auto RemoveVolumetricCloud(
			FVolumetricCloudSceneProxy* Proxy) -> void = 0;
	};
}
