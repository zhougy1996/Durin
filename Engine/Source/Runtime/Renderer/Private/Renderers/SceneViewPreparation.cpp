#include "Renderers/SceneViewPreparation.h"
#include "SceneView.h"

namespace Durin
{
	auto FitSceneViewToOutput(
		const FSceneView& View,
		uint32 Width,
		uint32 Height
	) -> FSceneView
	{
		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint <= 0.0f)
		{
			return RenderView;
		}

		uint32 ContentWidth = Width;
		uint32 ContentHeight = static_cast<uint32>(
			std::round(ContentWidth / RenderView.AspectRatioConstraint)
		);
		if (ContentHeight > Height)
		{
			ContentHeight = Height;
			ContentWidth = static_cast<uint32>(
				std::round(
					ContentHeight * RenderView.AspectRatioConstraint
				)
			);
		}
		RenderView.ViewportWidth = std::max(1u, ContentWidth);
		RenderView.ViewportHeight = std::max(1u, ContentHeight);
		RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
		RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		return RenderView;
	}

} // namespace Durin
