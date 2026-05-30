#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"

#include "DurinEngine.h"
#include "Engine/Engine.h"
#include "Mona/SceneViewport.h"
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
		std::shared_ptr<Mona::MWindow> RootWindow = std::make_shared<Mona::MWindow>();

		RootWindow->SetTitle("Mona");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {800.0f, 600.0f});

		std::shared_ptr<Mona::MFunctionWidget> DemoWidget = std::make_shared<Mona::MFunctionWidget>();
		DemoWidget->Construct([]() {
			Mona::ShowDemoWindow();
		});
		RootWindow->SetChild(DemoWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::BindMainViewportToWindow(RootWindow);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);

		std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, RootWindow);
		RootWindow->SetViewport(SceneViewport);
		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}
	}
}
