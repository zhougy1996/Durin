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

		virtual auto AddOrReplaceLight(
			FLightSceneId LightId,
			std::unique_ptr<FLightSceneProxy> Proxy) -> void = 0;
		virtual auto RemoveLight(FLightSceneId LightId) -> void = 0;

		virtual auto AddOrReplaceSkyBox(
			FSkyBoxSceneId SkyBoxId,
			std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> void = 0;
		virtual auto RemoveSkyBox(FSkyBoxSceneId SkyBoxId) -> void = 0;

		virtual auto AddOrReplaceVolumetricCloud(
			FVolumetricCloudSceneId CloudId,
			std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> void = 0;
		virtual auto RemoveVolumetricCloud(
			FVolumetricCloudSceneId CloudId) -> void = 0;
	};
}
