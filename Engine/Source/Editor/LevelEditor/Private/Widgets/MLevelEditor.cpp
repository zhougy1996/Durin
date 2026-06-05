#include "Widgets/MLevelEditor.h"

#include "Engine/Engine.h"
#include "Mona/SceneViewport.h"
#include "MonaUIBackend.h"
#include "Widgets/MViewport.h"

#include "imgui.h"

namespace Durin
{
	auto MLevelEditor::Construct() -> void
	{
		ViewportWidget = std::make_shared<Mona::MViewport>();
		SceneViewport = std::make_shared<FSceneViewport>(nullptr, ViewportWidget);
		ViewportWidget->SetViewport(SceneViewport);

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

		const FVector2f ViewportSize = UpdateViewportSize();
		UpdateDisplayedRenderTarget();

		const bool bDrawn = MonaUI::DrawTexture(DisplayedRenderTargetRHI.GetReference(), ViewportSize);
		if (!bDrawn)
		{
			ImGui::TextUnformatted("Viewport initializing...");
		}

		ImGui::End();
	}

	auto MLevelEditor::UpdateViewportSize() -> FVector2f
	{
		ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		AvailableSize.x = FMath::Max(8.0f, AvailableSize.x);
		AvailableSize.y = FMath::Max(8.0f, AvailableSize.y);

		const FVector2f ViewportSize = {AvailableSize.x, AvailableSize.y};
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->SetDesiredSize(ViewportSize);
		}
		if (SceneViewport != nullptr)
		{
			SceneViewport->UpdateRHIViewport();
		}

		return ViewportSize;
	}

	auto MLevelEditor::UpdateDisplayedRenderTarget() -> void
	{
		if (SceneViewport == nullptr)
		{
			return;
		}

		const FTextureRHIRef& RenderTargetRHI = SceneViewport->GetRenderTargetRHI();
		if (SceneViewport->IsRenderTargetReady() && RenderTargetRHI != nullptr)
		{
			DisplayedRenderTargetRHI = RenderTargetRHI;
		}
	}
}
