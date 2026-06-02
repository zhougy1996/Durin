#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"

#include "imgui.h"
#include "Widgets/MFunctionWidget.h"
#include "MonaImGuiBackend.h"

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
		FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		std::shared_ptr<Mona::MWindow> RootWindow = std::make_shared<Mona::MWindow>();
		std::shared_ptr<Mona::MFunctionWidget> EditorRootWidget = std::make_shared<Mona::MFunctionWidget>();

		RootWindow->SetTitle("Mona");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {800.0f, 600.0f});

		EditorRootWidget->Construct([]() {
			ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			ImGui::DockSpaceOverViewport(0, MainViewport, ImGuiDockNodeFlags_None);
			Mona::FMonaImGuiBackend::ShowDemoWindow();
		});
		RootWindow->SetChild(EditorRootWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
		Mona::FMonaImGuiBackend::Initialize();
		Mona::FMonaImGuiBackend::BindMainViewportToWindow(RootWindow);
	}
}
