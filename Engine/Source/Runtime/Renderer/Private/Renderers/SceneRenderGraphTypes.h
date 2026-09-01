#pragma once

#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderPlan.h"
#include "RDG.h"

namespace Durin
{
	enum class ESceneFeaturePurpose : uint8
	{
		None = 0,
		Production = 1 << 0,
		Debug = 1 << 1,
		Qualification = 1 << 2,
		Dependency = 1 << 3,
	};

	constexpr auto operator|(
		ESceneFeaturePurpose Left,
		ESceneFeaturePurpose Right) -> ESceneFeaturePurpose
	{
		return static_cast<ESceneFeaturePurpose>(
			static_cast<uint8>(Left) | static_cast<uint8>(Right));
	}

	struct FSceneFeatureDecision
	{
		ESceneFeaturePurpose Purposes = ESceneFeaturePurpose::None;

		[[nodiscard]] auto IsEnabled() const -> bool
		{
			return Purposes != ESceneFeaturePurpose::None;
		}

		[[nodiscard]] auto HasPurpose(ESceneFeaturePurpose Purpose) const -> bool
		{
			return (static_cast<uint8>(Purposes) & static_cast<uint8>(Purpose)) != 0;
		}
	};

	// Publishes the complete immutable feature policy for one scene submission.
	struct FSceneFrameFeaturePlan final
	{
		struct FAmbientOcclusion final : FSceneFeatureDecision
		{
			EGroundTruthAmbientOcclusionQuality Quality =
				EGroundTruthAmbientOcclusionQuality::FullResolution;
		};

		struct FContactVisibility final : FSceneFeatureDecision
		{
			FContactShadowVisibilityRenderer::FRouteDecision Decision;
		};

		struct FCloudShadow final : FSceneFeatureDecision
		{
			FVolumetricCloudShadowRenderer::FRouteDecision Decision;
		};

		struct FCloudSpatial final : FSceneFeatureDecision
		{
			FVolumetricCloudSpatialRenderer::FRouteDecision Decision;
			FIntPoint Extent{0, 0};
		};

		FSceneFeatureDecision Deferred;
		FSceneFeatureDecision GBuffer;
		FAmbientOcclusion AmbientOcclusion;
		FContactVisibility ContactVisibility;
		FCloudShadow CloudShadow;
		FCloudSpatial CloudSpatial;
		FSceneFeatureDecision GBufferDebug;
		FSceneFeatureDecision PostProcess;
		FSceneFeatureDecision EditorAssistance;

		[[nodiscard]] auto RequiresProductionDeferred() const -> bool
		{
			return Deferred.HasPurpose(ESceneFeaturePurpose::Production);
		}

		[[nodiscard]] auto RequiresIsolatedDeferred() const -> bool
		{
			return Deferred.HasPurpose(ESceneFeaturePurpose::Debug)
				|| Deferred.HasPurpose(ESceneFeaturePurpose::Qualification);
		}

		[[nodiscard]] auto RequiresDeferredInputs() const -> bool
		{
			return Deferred.IsEnabled() || AmbientOcclusion.IsEnabled();
		}
	};

	struct FResolvedSceneResources
	{
		FResolvedLighting Lighting;
		FResolvedReceiverGeometry Receiver;
		std::optional<FResolvedDirectionalShadow> DirectionalShadow;
		std::optional<FResolvedVolumetricCloud> VolumetricCloud;
	};

	struct FSceneRenderGraphComposition final
	{
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			DeferredParameters;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			ProductionDeferredParameters;
		FSceneColorPassResult SceneColorPublication;
		FPostProcessPassResult PostProcessPublication;
	};

	enum class ESceneRenderGraphExecutionStatus : uint8
	{
		CompileFailed,
		ExecutionFailed,
		Executed,
	};

} // namespace Durin
