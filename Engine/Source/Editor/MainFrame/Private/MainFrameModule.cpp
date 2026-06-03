#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"
#include "MonaImGuiBackend.h"
#include "MonaCoreGlobals.h"

#include "imgui.h"
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
		FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		check(Mona::GMonaUIBackend);
		Mona::FMonaImGuiBackend* ImGuiBackend = static_cast<Mona::FMonaImGuiBackend*>(Mona::GMonaUIBackend);
		std::shared_ptr<Mona::MWindow> RootWindow = std::make_shared<Mona::MWindow>();
		std::shared_ptr<Mona::MFunctionWidget> EditorRootWidget = std::make_shared<Mona::MFunctionWidget>();

		RootWindow->SetTitle("Mona");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {800.0f, 600.0f});

		EditorRootWidget->Construct([ImGuiBackend]() {
			ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			ImGui::DockSpaceOverViewport(0, MainViewport, ImGuiDockNodeFlags_None);
			ImGuiBackend->ShowDemoWindow();
		});
		RootWindow->SetChild(EditorRootWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
		ImGuiBackend->BindMainViewportToWindow(RootWindow);
	}
}
