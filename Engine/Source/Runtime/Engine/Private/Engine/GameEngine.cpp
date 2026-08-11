#include "Engine/GameEngine.h"

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Misc/Project.h"
#include "Mona.h"
#include "Mona/SceneViewport.h"
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
		StartupError.clear();

		std::shared_ptr<MWindow> GameWindow = std::make_shared<MWindow>();
		const FProjectInfo* Project = GetCurrentProject();
		GameWindow->SetTitle(Project ? Project->Name : "DurinGame");
		GameWindow->ReshapeWindow({100.0f, 100.0f}, {1280.0f, 720.0f});

		auto& MonaApp = Mona::FMonaApplication::Get();
		MonaApp.AddWindow(GameWindow, true);
		MonaApp.GetRenderer()->CreateViewport(GameWindow);

		std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, GameWindow);
		SetMainSceneViewport(SceneViewport);
		SetGameInputWindow(GameWindow->GetNativeWindow());
		SetGameInputEnabled(true);

		if (const FProjectInfo* CurrentProject = GetCurrentProject())
		{
			const FProjectGameSettingsStore SettingsStore = FProjectGameSettingsStore::ForProject(*CurrentProject);
			FProjectGameSettings Settings;
			const FProjectGameSettingsResult SettingsResult = SettingsStore.Load(Settings);
			if (!SettingsResult)
			{
				StartupError = SettingsResult.Message;
				DURIN_ERROR("Could not load project game settings: {}", StartupError);
				return;
			}
			const FNativeGameModeResolution GameMode = ResolveNativeGameMode(Settings);
			if (!GameMode)
			{
				StartupError = GameMode.Result.Message;
				DURIN_ERROR("Could not resolve native gameplay bootstrap: {}", StartupError);
				return;
			}
			if (!Settings.DefaultLevel.empty())
			{
				FSoftObjectPath SoftPath;
				TSoftObjectPtr<DLevel> DefaultLevel;
				DLevel* Level = nullptr;
				Asset::GetAssetRegistry().ScanMountedContent(Asset::EAssetRegistryScanMode::Incremental);
				if (FSoftObjectPath::TryCreate(Settings.DefaultLevel, SoftPath))
				{
					DefaultLevel.SetPath(std::move(SoftPath));
					const Asset::FAssetResult Result =
						Asset::LoadSoftObject(DefaultLevel, Level);
					if (Result && GetWorld()->SetCurrentLevel(Level))
					{
						const FWorldPlayResult PlayResult = GetWorld()->BeginPlay({.GameModeClass = GameMode.GameModeClass});
						if (!PlayResult)
						{
							StartupError = PlayResult.Message;
							DURIN_ERROR("Could not start default level '{}' with native gameplay: {}", Settings.DefaultLevel, StartupError);
						}
					}
					else
					{
						StartupError = Result.Message.empty() ? "Could not activate the default level." : Result.Message;
						DURIN_WARN("Could not start default level '{}': {}", Settings.DefaultLevel, StartupError);
					}
				}
				else
				{
					StartupError = std::format("Project Game.DefaultLevel '{}' is not a valid soft object path.", Settings.DefaultLevel);
					DURIN_WARN("{}", StartupError);
				}
			}
		}
	}
}
