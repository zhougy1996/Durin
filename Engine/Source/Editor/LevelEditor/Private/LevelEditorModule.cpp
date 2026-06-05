#include "LevelEditorModule.h"

#include "Engine/Engine.h"
#include "Mona/SceneViewport.h"
#include "MonaCoreGlobals.h"
#include "MonaImGuiBackend.h"
#include "Widgets/MFunctionWidget.h"
#include "Widgets/MViewport.h"

#include "imgui.h"

namespace Durin
{
	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	LEVELEDITOR_API auto FLevelEditorModule::CreateLevelEditorWidget() -> std::shared_ptr<Mona::MWidget>
	{
		std::shared_ptr<Mona::MViewport> ViewportWidget = std::make_shared<Mona::MViewport>();
		std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, ViewportWidget);
		ViewportWidget->SetViewport(SceneViewport);
		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}

		check(Mona::GMonaUIBackend);
		auto* ImGuiBackend = static_cast<Mona::FMonaImGuiBackend*>(Mona::GMonaUIBackend);
		std::shared_ptr<FTextureRHIRef> DisplayedRenderTargetRHI = std::make_shared<FTextureRHIRef>();

		std::shared_ptr<Mona::MFunctionWidget> LevelEditorWidget = std::make_shared<Mona::MFunctionWidget>();
		LevelEditorWidget->Construct([ViewportWidget, SceneViewport, ImGuiBackend, DisplayedRenderTargetRHI]() {
			ImGui::Begin("Level Editor");

			ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
			AvailableSize.x = FMath::Max(8.0f, AvailableSize.x);
			AvailableSize.y = FMath::Max(8.0f, AvailableSize.y);

			ViewportWidget->SetDesiredSize({AvailableSize.x, AvailableSize.y});
			SceneViewport->UpdateRHIViewport();

			const FTextureRHIRef& RenderTargetRHI = SceneViewport->GetRenderTargetRHI();
			if (SceneViewport->IsRenderTargetReady() && RenderTargetRHI != nullptr)
			{
				*DisplayedRenderTargetRHI = RenderTargetRHI;
			}

			const ImTextureID TextureID = ImGuiBackend->GetTextureID(*DisplayedRenderTargetRHI);
			if (TextureID != ImTextureID_Invalid)
			{
				ImGui::Image(TextureID, AvailableSize);
			}
			else
			{
				ImGui::TextUnformatted("Viewport initializing...");
			}

			ImGui::End();
		});
		return LevelEditorWidget;
	}
}
