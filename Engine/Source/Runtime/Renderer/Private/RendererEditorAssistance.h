#pragma once

#include "Misc/CoreTypes.h"
#include "RendererAPI.h"

namespace Durin::RendererEditorAssistance
{
	enum class EDrawOperation : uint8
	{
		EditorGrid,
		XRayGizmos,
		XRayOverlayLines,
		XRayOverlayIcons,
		VisibleGizmos,
		VisibleOverlayLines,
		VisibleOverlayIcons,
	};

	RENDERER_API auto GetDrawOrder() -> std::span<const EDrawOperation>;
} // namespace Durin::RendererEditorAssistance
