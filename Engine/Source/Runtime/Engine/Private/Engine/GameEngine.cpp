#include "Engine/GameEngine.h"

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Misc/Project.h"
#include "Mona.h"
#include "Mona/SceneViewport.h"
#include "Widgets/MWindow.h"
#include "Yaml/Yaml.h"

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

		std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, GameWindow);
		SetMainSceneViewport(SceneViewport);
		SetGameInputEnabled(true);

		if (const FProjectInfo* CurrentProject = GetCurrentProject())
		{
			FYamlDocument ProjectSettings;
			const std::string SettingsPath = CurrentProject->ProjectDir + "Configs/Project.yaml";
			if (ProjectSettings.LoadFromFile(SettingsPath))
			{
				const std::string DefaultLevel = ProjectSettings.GetRootView().GetView("Editor").GetView("DefaultLevel").GetString();
				FAssetPath LevelPath;
				DLevel* Level = nullptr;
				Asset::GetAssetRegistry().ScanMountedContent(Asset::EAssetRegistryScanMode::Incremental);
				if (!DefaultLevel.empty() && FAssetPath::TryCreate(DefaultLevel, LevelPath))
				{
					const Asset::FAssetResult Result = Asset::LoadAsset(LevelPath, Level);
					if (Result && GetWorld()->SetCurrentLevel(Level)) GetWorld()->BeginPlay();
					else DURIN_WARN("Could not start default level '{}': {}", DefaultLevel, Result.Message);
				}
			}
		}
	}
}
