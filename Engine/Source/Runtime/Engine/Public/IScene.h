#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"

namespace Durin
{
	class FPrimitiveSceneProxy;
	class FLightSceneProxy;
	class FSkyBoxSceneProxy;
	class FVolumetricCloudSceneProxy;
	struct FSkeletalPosePalette;
	struct FSplineMeshRenderDynamicData;
	template<typename TTag>
	struct TSceneId
	{
		uint64 Value = 0;
		explicit constexpr TSceneId(uint64 InValue = 0) : Value(InValue) {}
		auto operator<=>(const TSceneId&) const = default;
	};

	struct FPrimitiveSceneIdTag;
	struct FLightSceneIdTag;
	struct FSkyBoxSceneIdTag;
	struct FVolumetricCloudSceneIdTag;
	using FPrimitiveSceneId = TSceneId<FPrimitiveSceneIdTag>;
	using FLightSceneId = TSceneId<FLightSceneIdTag>;
	using FSkyBoxSceneId = TSceneId<FSkyBoxSceneIdTag>;
	using FVolumetricCloudSceneId = TSceneId<FVolumetricCloudSceneIdTag>;
	inline constexpr FPrimitiveSceneId InvalidPrimitiveSceneId;
	inline constexpr FLightSceneId InvalidLightSceneId;
	inline constexpr FSkyBoxSceneId InvalidSkyBoxSceneId;
	inline constexpr FVolumetricCloudSceneId InvalidVolumetricCloudSceneId;

	struct FSceneIdHash
	{
		template<typename TTag>
		auto operator()(TSceneId<TTag> Id) const -> size_t
		{
			return std::hash<uint64>{}(Id.Value);
		}
	};

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
		// Authored participation copied through the detached render-scene snapshot.
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
	};

	// Captures one immutable global cloud candidate without retaining reflected objects.
	struct FVolumetricCloudSceneData
	{
		FGuid PersistentId;
		std::string SelectionKey;
		uint64 InstanceId = 0;
		uint64 PublicationRevision = 0;
		int32 Priority = 0;
		bool bEnabled = true;
		bool bEligible = false;

		FRHITextureReferenceRef BaseDensityTexture;
		FRHITextureReferenceRef DetailDensityTexture;
		FRHITextureReferenceRef WeatherTexture;

		double MinimumZ = 1'500.0;
		double MaximumZ = 3'500.0;
		double MaximumDistance = 100'000.0;
		FVector3f BaseFrequency{0.00008f};
		FVector3f DetailFrequency{0.00032f};
		FVector3f WindOffset{0.0f};
		FVector2f WeatherFrequency{0.00004f};
		FVector2f WeatherOffset{0.0f};
		float Coverage = 0.55f;
		float DetailErosion = 0.30f;
		float Extinction = 0.0015f;
		float LightExtinction = 0.0020f;
		float Ambient = 0.12f;
	};

	// Defines the game-thread mutation boundary of a renderer-owned scene.
	class IScene
	{
	public:
		ENGINE_API IScene() = default;
		ENGINE_API virtual ~IScene() = default;

		virtual auto AddOrReplacePrimitive(
			FPrimitiveSceneId PrimitiveId,
			std::unique_ptr<FPrimitiveSceneProxy> Proxy,
			const FMatrix& Transform,
			bool bVisible = true
		) -> void = 0;

		virtual auto RemovePrimitive(FPrimitiveSceneId PrimitiveId) -> void = 0;

		virtual auto UpdatePrimitiveTransform(FPrimitiveSceneId PrimitiveId, const FMatrix& Transform) -> void = 0;
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
			FGuid PersistentId,
			std::string SelectionKey,
			std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> void = 0;
		virtual auto RemoveSkyBox(FSkyBoxSceneId SkyBoxId) -> void = 0;
		virtual auto GetActiveSkyBox_RenderThread(FSkyBoxSceneData& OutSkyBox) const -> bool = 0;
		virtual auto GetSkyBoxCount_RenderThread() const -> size_t = 0;

		virtual auto AddOrReplaceVolumetricCloud(
			FVolumetricCloudSceneId CloudId,
			uint64 PublicationRevision,
			std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> void = 0;
		virtual auto RemoveVolumetricCloud(
			FVolumetricCloudSceneId CloudId,
			uint64 ExpectedRevision) -> void = 0;
		virtual auto GetActiveVolumetricCloud_RenderThread(
			FVolumetricCloudSceneData& OutCloud) const -> bool = 0;
		virtual auto GetVolumetricCloudCount_RenderThread() const -> size_t = 0;
	};
}
