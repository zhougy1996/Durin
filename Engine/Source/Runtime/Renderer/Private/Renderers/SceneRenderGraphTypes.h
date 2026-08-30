#pragma once

#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderPlan.h"
#include "RDG.h"

namespace Durin
{
	enum class ESceneRenderRoute : uint8
	{
		Disabled,
		Fragment,
		Compute,
	};

	struct FSceneRenderTopology
	{
		uint32 Width = 0;
		uint32 Height = 0;
		bool bGBuffer = false;
		bool bGroundTruthAmbientOcclusion = false;
		ESceneRenderRoute ContactShadowVisibility = ESceneRenderRoute::Disabled;
		ESceneRenderRoute VolumetricCloudShadow = ESceneRenderRoute::Disabled;
		bool bIsolatedDeferred = false;
		bool bGBufferDebug = false;
		ESceneRenderRoute VolumetricCloud = ESceneRenderRoute::Disabled;
		bool bVolumetricCloudComposite = false;
		EGroundTruthAmbientOcclusionQuality AmbientOcclusionQuality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
		FIntPoint VolumetricCloudExtent{0, 0};

		[[nodiscard]] auto UsesContactShadowVisibilityFragment() const -> bool
		{
			return ContactShadowVisibility == ESceneRenderRoute::Fragment;
		}
		[[nodiscard]] auto UsesContactShadowVisibilityCompute() const -> bool
		{
			return ContactShadowVisibility == ESceneRenderRoute::Compute;
		}
		[[nodiscard]] auto UsesCloudShadowFragment() const -> bool
		{
			return VolumetricCloudShadow == ESceneRenderRoute::Fragment;
		}
		[[nodiscard]] auto UsesCloudShadowCompute() const -> bool
		{
			return VolumetricCloudShadow == ESceneRenderRoute::Compute;
		}
		[[nodiscard]] auto UsesCloudFragment() const -> bool
		{
			return VolumetricCloud == ESceneRenderRoute::Fragment;
		}
		[[nodiscard]] auto UsesCloudCompute() const -> bool
		{
			return VolumetricCloud == ESceneRenderRoute::Compute;
		}
	};

	struct FResolvedSceneResources
	{
		FResolvedLighting Lighting;
		FResolvedReceiverGeometry Receiver;
		std::optional<FResolvedDirectionalShadow> DirectionalShadow;
		std::optional<FResolvedVolumetricCloud> VolumetricCloud;
	};

	enum class ESceneRenderGraphExecutionStatus : uint8
	{
		CompileFailed,
		ExecutionFailed,
		Executed,
	};

	using FSceneRenderGraphExecute = std::function<ESceneRenderGraphExecutionStatus(
		FRDGBuilder&)>;
} // namespace Durin
