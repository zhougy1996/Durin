#include "Engine/GameEngine.h"

#include "CoreGlobals.h"
#include "Misc/Project.h"
#include "Mona.h"
#include "Mona/SceneViewport.h"
#include "Widgets/MViewport.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	DGameEngine::DGameEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DGameEngine::Init() -> void
	{
		DEngine::Init();

		std::shared_ptr<MWindow> GameWindow = std::make_shared<MWindow>();
		const FProjectInfo* Project = GetCurrentProject();
		GameWindow->SetTitle(Project ? Project->Name : "DurinGame");
		GameWindow->ReshapeWindow({100.0f, 100.0f}, {1280.0f, 720.0f});

		auto& MonaApp = Mona::FMonaApplication::Get();
		MonaApp.AddWindow(GameWindow, true);
		MonaApp.GetRenderer()->CreateViewport(GameWindow);

		std::shared_ptr<MViewport> GameViewportWidget = std::make_shared<MViewport>();
		GameWindow->SetContent(GameViewportWidget);

		std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, GameWindow);
		GameViewportWidget->SetViewportInterface(SceneViewport);
		SetMainSceneViewport(SceneViewport);
	}
}
