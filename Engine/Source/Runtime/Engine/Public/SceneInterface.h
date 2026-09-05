#pragma once

#include "EngineAPI.h"
#include "SceneTypes.h"

namespace Durin
{
	class DLightComponent;
	class DPrimitiveComponent;
	class DSkyBoxComponent;
	class DVolumetricCloudComponent;
	struct FMaterialRenderProxyBindingUpdate;
	struct FSplineMeshRenderDynamicData;

	// Defines the game-thread publication boundary of a renderer-owned scene.
	class FSceneInterface
	{
	public:
		ENGINE_API FSceneInterface() = default;
		ENGINE_API virtual ~FSceneInterface() = default;

		virtual auto AddPrimitive(DPrimitiveComponent* Primitive) -> void = 0;
		virtual auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void = 0;
		virtual auto AddLight(DLightComponent* Light) -> void = 0;
		virtual auto RemoveLight(DLightComponent* Light) -> void = 0;
		virtual auto AddSkyBox(DSkyBoxComponent* SkyBox) -> void = 0;
		virtual auto RemoveSkyBox(DSkyBoxComponent* SkyBox) -> void = 0;
		virtual auto AddVolumetricCloud(
			DVolumetricCloudComponent* Cloud
		) -> void = 0;
		virtual auto RemoveVolumetricCloud(
			DVolumetricCloudComponent* Cloud
		) -> void = 0;

		virtual auto Release() -> void = 0;

		virtual auto UpdatePrimitiveTransform(
			FPrimitiveSceneId PrimitiveId,
			const FMatrix& Transform
		) -> void = 0;
		virtual auto UpdatePrimitiveVisibility(
			FPrimitiveSceneId PrimitiveId,
			bool bVisible
		) -> void = 0;

		virtual auto UpdatePrimitiveMaterialBinding(
			FPrimitiveSceneId PrimitiveId,
			const FMaterialRenderProxyBindingUpdate& Update
		) -> void = 0;
		virtual auto UpdateSplineMeshDynamicData(
			FPrimitiveSceneId PrimitiveId,
			FSplineMeshRenderDynamicData DynamicData
		) -> void = 0;
	};
} // namespace Durin
