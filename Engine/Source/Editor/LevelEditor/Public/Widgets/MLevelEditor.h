#pragma once

#include "LevelEditorAPI.h"
#include "RHIResources.h"
#include "Widgets/MCompoundWidget.h"

namespace Durin
{
	class FSceneViewport;
	class MViewport;

	class MLevelEditor final : public MCompoundWidget
	{
	public:
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		auto DrawViewportPanel() -> void;
		auto UpdateViewportSize() -> FVector2f;
		auto UpdateDisplayedRenderTarget() -> void;

		std::shared_ptr<MViewport> ViewportWidget;
		std::shared_ptr<FSceneViewport> SceneViewport;
		FTextureRHIRef DisplayedRenderTargetRHI;
	};
}
