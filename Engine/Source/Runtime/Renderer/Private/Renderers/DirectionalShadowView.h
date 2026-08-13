#pragma once

#include "RendererAPI.h"

#include "IScene.h"
#include "Math/Box.h"
#include "SceneView.h"

#include <array>
#include <vector>

namespace Durin
{
	class FPrimitiveSceneInfo;
	class FScene;
	inline constexpr uint32 DirectionalShadowResolution = 2048;
	inline constexpr uint64 DirectionalShadowLogicalBytes =
		static_cast<uint64>(DirectionalShadowResolution)
		* DirectionalShadowResolution * sizeof(float);
	inline constexpr double DirectionalShadowDistance = 256.0;
	inline constexpr double DirectionalShadowCasterExtrusion = 256.0;
	inline constexpr uint32 DirectionalShadowGuardTexels = 2;
	inline constexpr float DirectionalShadowDepthBiasConstant = 1.25f;
	inline constexpr float DirectionalShadowDepthBiasSlope = 1.75f;
	inline constexpr float DirectionalShadowDepthBiasClamp = 4.0f;
	inline constexpr float DirectionalShadowReceiverBias = 0.0005f;

	struct FDirectionalShadowVolume
	{
		FVector3 Right{0.0};
		FVector3 Up{0.0};
		FVector3 Forward{0.0};
		FVector3 Minimum{0.0};
		FVector3 Maximum{0.0};
	};

	// Value-only result for one selected directional light and fitted scene view.
	struct FPreparedDirectionalShadowView
	{
		FLightSceneId LightId = InvalidLightSceneId;
		bool bEnabled = false;
		FMatrix LightViewMatrix{1.0};
		FMatrix LightProjectionMatrix{1.0};
		FMatrix LightViewProjectionMatrix{1.0};
		FMatrix WorldToShadowMatrix{1.0};
		FDirectionalShadowVolume CasterVolume;
		std::array<FVector3, 8> ReceiverCorners{};
		FVector2 TexelWorldSize{0.0};
		FSceneView CasterView;
	};

	struct FDirectionalShadowCasterCandidates
	{
		std::vector<const FPrimitiveSceneInfo*> StaticMeshes;
		std::vector<const FPrimitiveSceneInfo*> SplineMeshes;
		std::vector<const FPrimitiveSceneInfo*> SkeletalMeshes;
		std::vector<const FPrimitiveSceneInfo*> Terrains;
		size_t Submitted = 0;
		size_t Hidden = 0;
		size_t Culled = 0;
		size_t InvalidBoundsFallbacks = 0;
	};

	enum class EDirectionalShadowBoundsClassification : uint8
	{
		InsideOrIntersecting,
		Outside,
		InvalidBoundsFallback,
	};

	RENDERER_API auto TryPrepareDirectionalShadowView(
		const FSceneView& View,
		FLightSceneId LightId,
		const FDirectionalLightSceneData& Light,
		FPreparedDirectionalShadowView& OutShadow) -> bool;

	RENDERER_API auto ClassifyDirectionalShadowCasterBounds(
		const FPreparedDirectionalShadowView& Shadow,
		const FBox& WorldBounds) -> EDirectionalShadowBoundsClassification;

	// Starts from authoritative scene collections rather than camera visibility.
	RENDERER_API auto PrepareDirectionalShadowCasterCandidates(
		const FScene& Scene,
		const FPreparedDirectionalShadowView& Shadow,
		bool bDisableCulling = false) -> FDirectionalShadowCasterCandidates;

	RENDERER_API auto MakeDirectionalShadowSamplerDesc() -> FRHISamplerDesc;
}
