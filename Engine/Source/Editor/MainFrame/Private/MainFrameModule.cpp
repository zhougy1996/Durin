#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"

namespace Doge
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
		TSharedPtr<Mona::MWindow> RootWindow = std::make_shared<Mona::MWindow>();

		RootWindow->SetTitle("Mona");
		RootWindow->ResizeWindow({800.0f, 600.0f});

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
	}
}