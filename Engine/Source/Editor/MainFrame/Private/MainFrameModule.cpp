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

		RootWindow->SetTitle("Durin Editor");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {1280.0f, 800.0f});

		EditorRootWidget->Construct([LevelEditorWidget]() {
			if (LevelEditorWidget != nullptr)
			{
				LevelEditorWidget->Draw();
			}
		});
		RootWindow->SetContent(EditorRootWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
	}
} // namespace Durin
