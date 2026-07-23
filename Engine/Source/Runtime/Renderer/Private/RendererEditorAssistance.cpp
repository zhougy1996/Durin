#include "RendererEditorAssistance.h"

namespace Durin::RendererEditorAssistance
{
	auto GetDrawOrder() -> std::span<const EDrawOperation>
	{
		static constexpr std::array DrawOrder{
			EDrawOperation::EditorGrid,
			EDrawOperation::XRayGizmos,
			EDrawOperation::XRayOverlayLines,
			EDrawOperation::XRayOverlayIcons,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::VisibleOverlayLines,
			EDrawOperation::VisibleOverlayIcons,
		};
		return DrawOrder;
	}
} // namespace Durin::RendererEditorAssistance
