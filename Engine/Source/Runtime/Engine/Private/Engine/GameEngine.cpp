#include "Engine/GameEngine.h"

#include "Misc/AppConfigCache.h"
#include "Mona.h"
#include "Mona/SceneViewport.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	auto DGameEngine::Init() -> void
	{
		DEngine::Init();

		std::shared_ptr<Mona::MWindow> GameWindow = std::make_shared<Mona::MWindow>();
		GameWindow->SetTitle(GAppConfig.GetStringValue("AppName"));
		GameWindow->ReshapeWindow({100.0f, 100.0f}, {1280.0f, 720.0f});

		auto& MonaApp = Mona::FMonaApplication::Get();
		MonaApp.AddWindow(GameWindow, true);
		MonaApp.GetRenderer()->CreateViewport(GameWindow);

		std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, GameWindow);
		GameWindow->SetViewport(SceneViewport);
		SetMainSceneViewport(SceneViewport);
	}
}
