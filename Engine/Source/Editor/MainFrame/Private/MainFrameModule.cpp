#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"
#include "MonaImGui.h"

#include "Widgets/MFunctionWidget.h"

namespace Durin
{
	IMPLEMENT_MODULE(FMainFrameModule, MainFrame)

	auto FMainFrameModule::StartupModule() -> void
	{
	}

	auto FMainFrameModule::ShutdownModule() -> void
	{
	}

	auto FMainFrameModule::CreateDefaultMainFrame() -> void
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		auto RootWindow = std::make_shared<MWindow>();
		MonaImGui::BindMainViewportToWindow(RootWindow);

		auto EditorRootWidget = std::make_shared<MFunctionWidget>();
		std::shared_ptr<MWidget> LevelEditorWidget = LevelEditorModule.CreateLevelEditorWidget();

		RootWindow->SetTitle("Mona");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {800.0f, 600.0f});

		EditorRootWidget->Construct([LevelEditorWidget]() {
			ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			ImGui::DockSpaceOverViewport(0, MainViewport, ImGuiDockNodeFlags_None);
			if (LevelEditorWidget != nullptr)
			{
				LevelEditorWidget->Draw();
			}
		});
		RootWindow->SetContent(EditorRootWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
	}
}
