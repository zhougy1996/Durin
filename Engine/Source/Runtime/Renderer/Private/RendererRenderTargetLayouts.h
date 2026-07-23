#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

namespace Durin::RendererRenderTargetLayouts
{
	enum class EViewportOutput : uint8
	{
		Offscreen,
		Present,
	};

	RENDERER_API auto MakeSceneTargets() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeScenePostProcessOutput() -> FRHIRenderTargetLayout;
	RENDERER_API auto MakeFinalEditorAssistanceOutput(EViewportOutput Output) -> FRHIRenderTargetLayout;
} // namespace Durin::RendererRenderTargetLayouts
