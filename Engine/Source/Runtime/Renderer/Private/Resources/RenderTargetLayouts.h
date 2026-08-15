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

	// Scene Color plus the selected directional light's post-shadow direct
	// contribution, followed by the preserved scene depth attachment.
	RENDERER_API auto MakeSceneTargets() -> FRHIRenderTargetLayout;
	// Four stored color attachments plus D32 depth for the qualified opaque and
	// masked geometry transport.
	RENDERER_API auto MakeGBufferTargets() -> FRHIRenderTargetLayout;
	// One D32 depth attachment that is cleared for shadow rendering and
	// published for fragment-shader comparison sampling when the pass ends.
	RENDERER_API auto MakeDirectionalShadowDepth() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeScenePostProcessOutput() -> FRHIRenderTargetLayout;
	// One color-only RGBA16_FLOAT target that is cleared and published for
	// sampling; used by the contact-shadow pass writing corrected Scene Color.
	RENDERER_API auto MakeContactShadowOutput() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeFinalScenePostProcessOutput(EViewportOutput Output)
		-> FRHIRenderTargetLayout;
	RENDERER_API auto MakeEditorAssistanceOutput(EViewportOutput Output) -> FRHIRenderTargetLayout;
} // namespace Durin::RenderTargetLayouts
