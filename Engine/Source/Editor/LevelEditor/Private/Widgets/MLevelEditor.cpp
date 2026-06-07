#include "Widgets/MLevelEditor.h"

#include "Engine/Engine.h"
#include "MonaImGui.h"
#include "Mona/SceneViewport.h"
#include "Widgets/MViewport.h"

namespace Durin
{
	auto MLevelEditor::Construct() -> void
	{
		ViewportWidget = std::make_shared<MViewport>();
		const std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, ViewportWidget);
		ViewportWidget->SetViewportInterface(SceneViewport);

		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}
	}

	auto MLevelEditor::Draw() -> void
	{
		DrawViewportPanel();
	}

	auto MLevelEditor::DrawViewportPanel() -> void
	{
		ImGui::Begin("Level Editor");

		UpdateViewportSize();

		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

		if (ViewportWidget != nullptr)
		{
			ViewportWidget->Draw();
		}
		if (ViewportWidget == nullptr || !ViewportWidget->WasTextureDrawn())
		{
			ImGui::TextUnformatted("Viewport initializing...");
		}

		ImGui::End();
	}

	auto MLevelEditor::UpdateViewportSize() -> void
	{
		ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		AvailableSize.x = FMath::Max(8.0f, AvailableSize.x);
		AvailableSize.y = FMath::Max(8.0f, AvailableSize.y);

		const FVector2f ViewportSize = {AvailableSize.x, AvailableSize.y};
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->SetDesiredSize(ViewportSize);
		}
	}
}
