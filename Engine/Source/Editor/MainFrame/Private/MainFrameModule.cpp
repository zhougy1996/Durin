#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"
#include "MonaImGui.h"
#include "Application/GenericApplication.h"
#include "Misc/Paths.h"
#include "Yaml/Yaml.h"

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
		FIntPoint WindowSize{1280, 800};
		float UIScale = 1.0f;
		bool bWindowMaximized = true;
		const std::vector<FMonitorInfo> Monitors = EnumerateMonitors();
		if (!Monitors.empty())
		{
			const FIntPoint WorkSize = Monitors.front().WorkSize;
			WindowSize = {std::min(1600, static_cast<int32>(WorkSize.x * 0.9f)), std::min(1000, static_cast<int32>(WorkSize.y * 0.9f))};
			UIScale = WorkSize.y >= 1800 ? 1.5f : WorkSize.y >= 1300 ? 1.25f : 1.0f;
		}
		FYamlDocument SettingsDocument;
		if (SettingsDocument.LoadFromFile(FPaths::LaunchDir() + "LevelEditorSession.yaml"))
		{
			const FYamlNodeView Display = SettingsDocument.GetRootView().GetView("Display");
			WindowSize.x = static_cast<int32>(Display.GetView("WindowWidth").GetInt(WindowSize.x));
			WindowSize.y = static_cast<int32>(Display.GetView("WindowHeight").GetInt(WindowSize.y));
			UIScale = static_cast<float>(Display.GetView("UIScale").GetDouble(UIScale));
			bWindowMaximized = Display.GetView("WindowMaximized").GetBool(true);
		}
		MonaImGui::SetGlobalUIScale(UIScale);
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		auto RootWindow = std::make_shared<MWindow>();
		MonaImGui::BindMainViewportToWindow(RootWindow);

		auto EditorRootWidget = std::make_shared<MFunctionWidget>();
		std::shared_ptr<MWidget> LevelEditorWidget = LevelEditorModule.CreateLevelEditorWidget();

		RootWindow->SetTitle("Durin Editor");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		EditorRootWidget->Construct([LevelEditorWidget]() {
			if (LevelEditorWidget != nullptr)
			{
				LevelEditorWidget->Draw();
			}
		});
		RootWindow->SetContent(EditorRootWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);

		if (bWindowMaximized)
		{
			RootWindow->MaximizeWindow();
		}
	}
} // namespace Durin
