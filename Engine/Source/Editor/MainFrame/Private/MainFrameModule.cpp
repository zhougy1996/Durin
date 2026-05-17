#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"

namespace Durin
{
	IMPLEMENT_MODULE(FMainFrameModule, MainFrame)

	auto FMainFrameModule::StartupModule() -> void
	{
	}

	auto FMainFrameModule::ShutdownModule() -> void
	{
	}

	class MMainFrame final : public Mona::MWidget
	{
	private:
		auto Draw() -> void override
		{
			//Mona::Text(FString("Test"));
		}
	};

	auto FMainFrameModule::CreateDefaultMainFrame() -> void
	{
		FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		std::shared_ptr<Mona::MWindow> RootWindow = std::make_shared<Mona::MWindow>();

		RootWindow->SetTitle("Mona");
		RootWindow->ResizeWindow({800.0f, 600.0f});

		std::shared_ptr<MMainFrame> MainFrame = std::make_shared<MMainFrame>();
		RootWindow->SetChild(MainFrame);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
	}
}