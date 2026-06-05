#pragma once

#include "LevelEditorAPI.h"
#include "RHIResources.h"
#include "Widgets/MCompoundWidget.h"

namespace Durin
{
	class FSceneViewport;
}

namespace Durin::Mona
{
	class MViewport;
}

namespace Durin
{
	class MLevelEditor final : public Mona::MCompoundWidget
	{
	public:
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		auto DrawViewportPanel() -> void;
		auto UpdateViewportSize() -> FVector2f;
		auto UpdateDisplayedRenderTarget() -> void;

		std::shared_ptr<Mona::MViewport> ViewportWidget;
		std::shared_ptr<FSceneViewport> SceneViewport;
		FTextureRHIRef DisplayedRenderTargetRHI;
	};
}
