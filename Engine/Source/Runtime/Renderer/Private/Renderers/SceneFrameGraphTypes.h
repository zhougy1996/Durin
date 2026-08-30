#pragma once

#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderPlan.h"
#include "RenderGraph.h"

namespace Durin
{
	enum class ESceneFrameRoute : uint8
	{
		Disabled,
		Fragment,
		Compute,
	};

	struct FSceneFrameTopology
	{
		uint32 Width = 0;
		uint32 Height = 0;
		bool bGBuffer = false;
		bool bGroundTruthAmbientOcclusion = false;
		ESceneFrameRoute ContactShadowVisibility = ESceneFrameRoute::Disabled;
		ESceneFrameRoute VolumetricCloudShadow = ESceneFrameRoute::Disabled;
		bool bIsolatedDeferred = false;
		bool bGBufferDebug = false;
		ESceneFrameRoute VolumetricCloud = ESceneFrameRoute::Disabled;
		bool bVolumetricCloudComposite = false;
		EGroundTruthAmbientOcclusionQuality AmbientOcclusionQuality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
		FIntPoint VolumetricCloudExtent{0, 0};

		[[nodiscard]] auto UsesContactShadowVisibilityFragment() const -> bool
		{
			return ContactShadowVisibility == ESceneFrameRoute::Fragment;
		}
		[[nodiscard]] auto UsesContactShadowVisibilityCompute() const -> bool
		{
			return ContactShadowVisibility == ESceneFrameRoute::Compute;
		}
		[[nodiscard]] auto UsesCloudShadowFragment() const -> bool
		{
			return VolumetricCloudShadow == ESceneFrameRoute::Fragment;
		}
		[[nodiscard]] auto UsesCloudShadowCompute() const -> bool
		{
			return VolumetricCloudShadow == ESceneFrameRoute::Compute;
		}
		[[nodiscard]] auto UsesCloudFragment() const -> bool
		{
			return VolumetricCloud == ESceneFrameRoute::Fragment;
		}
		[[nodiscard]] auto UsesCloudCompute() const -> bool
		{
			return VolumetricCloud == ESceneFrameRoute::Compute;
		}
	};

	struct FResolvedSceneFrame
	{
		FResolvedLighting Lighting;
		FResolvedReceiverGeometry Receiver;
		std::optional<FResolvedDirectionalShadow> DirectionalShadow;
		std::optional<FResolvedVolumetricCloud> VolumetricCloud;
	};

	template <typename TResult>
	struct TSceneFrameGraphValue
	{
		TRenderGraphValueHandle<TResult> Handle;
	};

	enum class ESceneFrameGraphExecutionStatus : uint8
	{
		CompileFailed,
		ExecutionFailed,
		Executed,
	};

	using FSceneFrameGraphExecute = std::function<ESceneFrameGraphExecutionStatus(
		FRenderGraphBuilder&)>;
} // namespace Durin
