#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

namespace Durin::RenderTargetLayouts
{
	// Identifies the renderer output layout used for offscreen or presented views.
	enum class EViewportOutput : uint8
	{
		Offscreen,
		Present,
	};

	// Scene Color followed by the preserved scene depth attachment.
	RENDERER_API auto MakeSceneTargets() -> FRHIRenderTargetLayout;
	// Four stored color attachments plus D32 depth for the qualified opaque and
	// masked geometry transport.
	RENDERER_API auto MakeGBufferTargets() -> FRHIRenderTargetLayout;
	// One D32 depth attachment that is cleared for shadow rendering and
	// published for fragment-shader comparison sampling when the pass ends.
	RENDERER_API auto MakeDirectionalShadowDepth() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeScenePostProcessOutput() -> FRHIRenderTargetLayout;
	// One cleared R8_UNORM contact-visibility target published for sampling.
	RENDERER_API auto MakeContactVisibilityOutput() -> FRHIRenderTargetLayout;
	// Cloud radiance/transmittance target and linear ping-pong scene composite.
	RENDERER_API auto MakeVolumetricCloudOutput() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeVolumetricCloudComposite() -> FRHIRenderTargetLayout;
	// One on-demand RGBA16_FLOAT target used only by GBuffer diagnostics.
	RENDERER_API auto MakeGBufferDebugOutput() -> FRHIRenderTargetLayout;
	// Isolated M3 RGBA16_FLOAT qualification output, cleared per view and left
	// shader-readable for capture/A-B without becoming the presented Scene Color.
	RENDERER_API auto MakeDeferredDirectionalOutput() -> FRHIRenderTargetLayout;
	// One cleared R8_UNORM raw visibility target published for sampling/capture.
	RENDERER_API auto MakeGroundTruthAmbientOcclusionOutput()
		-> FRHIRenderTargetLayout;
	// Clears/loads the authoritative hybrid Scene Color while preserving
	// GBuffer depth across sky bootstrap, deferred lighting, and retained forward.
	RENDERER_API auto MakeHybridSceneBootstrap() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeHybridDeferredOutput() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeHybridRetainedForward() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeHybridSortedTranslucency() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeFinalScenePostProcessOutput(EViewportOutput Output)
		-> FRHIRenderTargetLayout;
	RENDERER_API auto MakeEditorAssistanceOutput(EViewportOutput Output) -> FRHIRenderTargetLayout;
} // namespace Durin::RenderTargetLayouts
