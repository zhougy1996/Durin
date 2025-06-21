#include "MainFrameModule.h"

#include "Mona.h"

auto FMainFrameModule::StartupModule() -> void
{
}

auto FMainFrameModule::ShutdownModule() -> void
{
}

auto FMainFrameModule::CreateDefaultMainFrame() -> void
{
	// FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<MWindow> RootWindow = std::make_shared<MWindow>();

	RootWindow->SetTitle("Mona");
	RootWindow->ResizeWindow({800.0f, 600.0f});

	FMonaApplication::Get().AddWindow(RootWindow, true);
	FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
}