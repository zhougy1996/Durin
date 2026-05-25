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

	auto FMainFrameModule::CreateDefaultMainFrame() -> void
	{
		FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		std::shared_ptr<Mona::MWindow> RootWindow = std::make_shared<Mona::MWindow>();

		RootWindow->SetTitle("Mona");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {800.0f, 600.0f});

		class MMainFrame final : public Mona::MWidget
		{
		public:
			auto Draw() -> void override
			{
				Mona::ShowDemoWindow();
			}
		};

		auto MainFrame = std::make_shared<MMainFrame>();
		RootWindow->SetChild(MainFrame);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::BindMainViewportToWindow(RootWindow);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);
	}
}
