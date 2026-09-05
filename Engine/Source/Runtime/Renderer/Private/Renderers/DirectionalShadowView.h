#pragma once

#include "RendererAPI.h"

#include "Math/Box.h"
#include "Rendering/LightSceneProxy.h"
#include "SceneTypes.h"
#include "SceneView.h"

#include <array>
#include <vector>

namespace Durin
{
	class FPrimitiveSceneInfo;
	class FScene;
	inline constexpr uint32 DirectionalShadowResolution = 2048;
	inline constexpr uint32 DirectionalShadowCascadeCount = 3;
	inline constexpr uint64 DirectionalShadowLogicalBytes =
		static_cast<uint64>(DirectionalShadowResolution)
		* DirectionalShadowResolution * sizeof(float)
		* DirectionalShadowCascadeCount;
	inline constexpr double DirectionalShadowDistance = 256.0;
	inline constexpr double DirectionalShadowCasterExtrusion = 256.0;
	inline constexpr double DirectionalShadowSplitLambda = 0.65;
	inline constexpr double DirectionalShadowTransitionFraction = 0.10;
	inline constexpr uint32 DirectionalShadowLowGuardTexels = 2;
	inline constexpr uint32 DirectionalShadowMediumGuardTexels = 2;
	inline constexpr uint32 DirectionalShadowHighGuardTexels = 3;
	inline constexpr float DirectionalShadowDepthBiasConstant = 1.25f;
	inline constexpr float DirectionalShadowDepthBiasSlope = 1.75f;
	inline constexpr float DirectionalShadowDepthBiasClamp = 4.0f;
	inline constexpr float DirectionalShadowReceiverBias = 0.0005f;
	inline constexpr float DirectionalShadowMaximumReceiverWorldBias = 0.0f;
	inline constexpr float DirectionalShadowMaximumNormalOffset = 0.0f;
	inline constexpr float DirectionalShadowMaximumTotalWorldBias = 0.08f;

	struct FDirectionalShadowBias
	{
		float RasterConstant = DirectionalShadowDepthBiasConstant;
		float RasterSlope = DirectionalShadowDepthBiasSlope;
		float RasterClamp = DirectionalShadowDepthBiasClamp;
		float ReceiverWorld = 0.0f;
		float NormalWorld = 0.0f;
		float NormalizedRasterSeparation = 0.0f;
		bool bUsedFallback = false;
		bool bTotalClamped = false;
	};

	struct FDirectionalShadowVolume
	{
		FVector3 Right{0.0};
		FVector3 Up{0.0};
		FVector3 Forward{0.0};
		FVector3 Minimum{0.0};
		FVector3 Maximum{0.0};
	};

	struct FDirectionalShadowFilter
	{
		EDirectionalShadowFilterQuality Quality =
			EDirectionalShadowFilterQuality::Low;
		uint32 ComparisonOperations = 1;
		uint32 GuardTexels = DirectionalShadowLowGuardTexels;
		float FootprintRadiusTexels = 0.5f;
		bool bUsedInvalidQualityFallback = false;
	};

	// Owns one independently fitted receiver slice and its caster-view contract.
	struct FPreparedDirectionalShadowCascade
	{
		bool bEnabled = false;
		uint32 Layer = 0;
		double NearDepth = 0.0;
		double FarDepth = 0.0;
		double TransitionStartDepth = 0.0;
		FMatrix LightViewMatrix{1.0};
		FMatrix LightProjectionMatrix{1.0};
		FMatrix LightViewProjectionMatrix{1.0};
		FMatrix WorldToShadowMatrix{1.0};
		FDirectionalShadowVolume CasterVolume;
		std::array<FVector3, 8> ReceiverCorners{};
		FVector2 TexelWorldSize{0.0};
		FDirectionalShadowBias Bias;
		FDirectionalShadowFilter Filter;
		FSceneView CasterView;
	};

	// Value-only result for one selected light and one immutable cascade candidate.
	struct FPreparedDirectionalShadowView
	{
		FLightSceneId LightId = InvalidLightSceneId;
		bool bEnabled = false;
		EDirectionalShadowCandidate Candidate =
			EDirectionalShadowCandidate::SingleMap;
		uint32 CascadeCount = 0;
		std::array<double, DirectionalShadowCascadeCount + 1> SplitDepths{};
		FVector4 ViewDepthTransform{0.0};
		FVector3 LightDirection{0.0, 0.0, -1.0};
		EDirectionalShadowDiagnosticMode DiagnosticMode =
			EDirectionalShadowDiagnosticMode::Lit;
		std::array<FPreparedDirectionalShadowCascade,
			DirectionalShadowCascadeCount> Cascades{};
	};

	struct FDirectionalShadowCasterCandidates
	{
		std::vector<const FPrimitiveSceneInfo*> StaticMeshes;
		std::vector<const FPrimitiveSceneInfo*> SplineMeshes;
		std::vector<const FPrimitiveSceneInfo*> SkeletalMeshes;
		size_t Submitted = 0;
		size_t Hidden = 0;
		size_t Culled = 0;
		size_t InvalidBoundsFallbacks = 0;
	};

	enum class EDirectionalShadowCasterKind : uint8
	{
		StaticMesh,
		SplineMesh,
		SkeletalMesh,
	};

	// Non-owning frame-local caster identity and membership snapshot.
	struct FDirectionalShadowCasterRecord
	{
		const FPrimitiveSceneInfo* SceneInfo = nullptr;
		EDirectionalShadowCasterKind Kind =
			EDirectionalShadowCasterKind::StaticMesh;
		uint8 CascadeMask = 0;
		uint8 InvalidBoundsFallbackMask = 0;
	};

	// Owns one authoritative scene traversal and cascade-local reference lists.
	struct FDirectionalShadowCasterTable
	{
		std::vector<FDirectionalShadowCasterRecord> Records;
		std::array<FDirectionalShadowCasterCandidates,
			DirectionalShadowCascadeCount> Cascades;
		size_t SceneTraversals = 0;
		size_t UniqueSubmitted = 0;
		size_t UniqueHidden = 0;
		size_t UniqueEligibleStaticMeshes = 0;
		size_t UniqueEligibleSplineMeshes = 0;
		size_t UniqueEligibleSkeletalMeshes = 0;
		size_t CascadeClassificationTests = 0;
		size_t MembershipPopcount = 0;
		size_t TemporaryBytes = 0;
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
		const FPreparedDirectionalShadowCascade& Cascade,
		const FBox& WorldBounds) -> EDirectionalShadowBoundsClassification;

	// Classifies authoritative scene primitives against every enabled cascade in
	// one traversal. Returned references remain valid only for the scene snapshot.
	RENDERER_API auto PrepareDirectionalShadowCasterTable(
		const FScene& Scene,
		const FPreparedDirectionalShadowView& Shadow,
		bool bDisableCulling = false) -> FDirectionalShadowCasterTable;

	RENDERER_API auto SelectDirectionalShadowCascade(
		const FPreparedDirectionalShadowView& Shadow,
		double ReceiverDepth,
		uint32& OutCascade,
		uint32& OutNearCascade,
		double& OutTransitionWeight) -> bool;

	RENDERER_API auto MakeDirectionalShadowSamplerDesc() -> FRHISamplerDesc;
	RENDERER_API auto PrepareDirectionalShadowFilter(
		EDirectionalShadowFilterQuality Quality) -> FDirectionalShadowFilter;
	RENDERER_API auto CalculateDirectionalShadowBias(
		const FVector2& TexelWorldSize,
		double SurfaceLightCosine = 1.0) -> FDirectionalShadowBias;
}
