#pragma once

#include "IRendererModule.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/VolumetricCloudSpatialRenderer.h"

namespace Durin
{
	enum class EScenePassStatus : uint8
	{
		NotRequested,
		Complete,
		Failed
	};

	struct FDirectionalShadowPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete;
		}
	};

	struct FGBufferPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		bool bRenderedGeometry = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete;
		}
	};

	struct FGroundTruthAmbientOcclusionPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		bool bHalfResolution = false;
		bool bRawDiagnosticUsesScratch = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete;
		}
	};

	enum class EContactShadowVisibilityPassRoute : uint8
	{
		None,
		Compute,
		Fragment
	};

	struct FContactShadowVisibilityPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		EContactShadowVisibilityPassRoute Route =
			EContactShadowVisibilityPassRoute::None;
		bool bDebug = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete
				&& Route != EContactShadowVisibilityPassRoute::None;
		}
	};

	enum class EVolumetricCloudShadowPassRoute : uint8
	{
		None,
		Compute,
		Fragment
	};

	struct FVolumetricCloudShadowPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		EVolumetricCloudShadowPassRoute Route =
			EVolumetricCloudShadowPassRoute::None;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete
				&& Route != EVolumetricCloudShadowPassRoute::None;
		}
	};

	struct FIsolatedDeferredPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		bool bOutputValid = false;
	};

	struct FVolumetricCloudPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		bool bCompositeOutputValid = false;
	};

	struct FVolumetricCloudSpatialPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		FVolumetricCloudSpatialRenderer::ERoute Route =
			FVolumetricCloudSpatialRenderer::ERoute::Disabled;
	};

	struct FSceneColorPassResult
	{
		ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
		bool bUsesVolumetricCloudComposite = false;
		FVolumetricCloudPassResult VolumetricCloud;

		[[nodiscard]] auto IsSuccess() const -> bool
		{
			return Result == ERenderViewResult::Success;
		}
	};

	struct FPostProcessPassResult
	{
		ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
		bool bEditorAssistance = false;
	};

	struct FSceneRenderOutcome
	{
		FDirectionalShadowPassResult DirectionalShadow;
		FGBufferPassResult GBuffer;
		FGroundTruthAmbientOcclusionPassResult AmbientOcclusion;
		FContactShadowVisibilityPassResult ContactShadow;
		FVolumetricCloudShadowPassResult VolumetricCloudShadow;
		FIsolatedDeferredPassResult IsolatedDeferred;
		FSceneColorPassResult SceneColor;
		FPostProcessPassResult PostProcess;
	};
} // namespace Durin
