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

	RENDERER_API auto MakeSceneTargets() -> FRHIRenderTargetLayout;
	// One D32 depth attachment that is cleared for shadow rendering and
	// published for fragment-shader comparison sampling when the pass ends.
	RENDERER_API auto MakeDirectionalShadowDepth() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeScenePostProcessOutput() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeFinalScenePostProcessOutput(EViewportOutput Output)
		-> FRHIRenderTargetLayout;
	RENDERER_API auto MakeEditorAssistanceOutput(EViewportOutput Output) -> FRHIRenderTargetLayout;
} // namespace Durin::RenderTargetLayouts
