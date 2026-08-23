#pragma once

#include "IRendererModule.h"
#include "Renderers/GBufferRenderer.h"

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
		FRHITexture* Texture = nullptr;
		FRHISampler* Sampler = nullptr;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete
				&& Texture != nullptr;
		}
	};

	struct FGBufferPassResult
	{
		const FGBufferRenderer::FTargets* Targets = nullptr;
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		bool bRenderedGeometry = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete && Targets != nullptr;
		}
	};

	struct FGroundTruthAmbientOcclusionPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		FRHITexture* Raw = nullptr;
		FRHITexture* Filtered = nullptr;
		FRHITexture* Resolved = nullptr;
		FRHITexture* Selector = nullptr;
		bool bHalfResolution = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete
				&& Raw != nullptr && Filtered != nullptr
				&& Resolved != nullptr;
		}
	};

	struct FContactShadowPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		FRHITexture* Visibility = nullptr;
		bool bDebug = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete
				&& Visibility != nullptr;
		}
	};

	struct FVolumetricCloudShadowPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		FRHITexture* Visibility = nullptr;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete
				&& Visibility != nullptr;
		}
	};

	struct FIsolatedDeferredPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		FRHITexture* Output = nullptr;
	};

	struct FVolumetricCloudPassResult
	{
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		FRHITexture* SceneColor = nullptr;
	};

	struct FSceneColorPassResult
	{
		ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
		FRHITexture* SceneColor = nullptr;
		FVolumetricCloudPassResult VolumetricCloud;

		[[nodiscard]] auto IsSuccess() const -> bool
		{
			return Result == ERenderViewResult::Success
				&& SceneColor != nullptr;
		}
	};

	struct FPostProcessPassResult
	{
		ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
		FRHITexture* Input = nullptr;
		bool bEditorAssistance = false;
	};

	struct FSceneFrameOutcome
	{
		FDirectionalShadowPassResult DirectionalShadow;
		FGBufferPassResult GBuffer;
		FGroundTruthAmbientOcclusionPassResult AmbientOcclusion;
		FContactShadowPassResult ContactShadow;
		FVolumetricCloudShadowPassResult VolumetricCloudShadow;
		FIsolatedDeferredPassResult IsolatedDeferred;
		FSceneColorPassResult SceneColor;
		FPostProcessPassResult PostProcess;
	};
} // namespace Durin
