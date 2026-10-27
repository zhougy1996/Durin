#include "Engine/GameEngine.h"
#include "Engine/MapChange.h"

#include "Asset/Asset.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialProgramPreparation.h"
#include "CoreGlobals.h"
#include "Engine/Level.h"
#include "Engine/Actor.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Misc/Project.h"
#include "Modules/ModuleManager.h"
#include "Application/MonaApplication.h"
#include "Client/SceneViewport.h"
#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"
#include "Window/GenericWindow.h"

namespace Durin
{
	DGameEngine::DGameEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DGameEngine::Init(const FEngineInitContext& Context)
		-> bool
	{
		if (!DEngine::Init(Context))
			return false;
		StartupError.clear();
		if (!Context.ProjectAdmissionError.empty())
		{
			StartupError = Context.ProjectAdmissionError;
			return true;
		}
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			for (const std::string& ModuleName : Project->EnabledRootModules)
				if (!FModuleManager::Get().LoadModule(FName(ModuleName)))
				{
					DURIN_ERROR("Game initialization could not load project module '{}'.", ModuleName);
					return false;
				}
		}

		if (const auto Content = InitializeProjectContent(); !Content)
		{
			StartupError = Content.error();
			return true;
		}
		std::shared_ptr<MWindow> GameWindow = Context.StartupWindow;
		if (!GameWindow || !GameWindow->GetNativeWindow())
		{
			DURIN_ERROR("Game initialization requires a native startup window.");
			return false;
		}
		const FProjectInfo* Project = GetCurrentProject();
		GameWindow->SetTitle(Project ? Project->Name : "DurinGame");
		GameWindow->ReshapeWindow({100.0f, 100.0f}, {1280.0f, 720.0f});

		auto& MonaApp = Mona::FMonaApplication::Get();
		MonaApp.GetRenderer()->CreateViewport(GameWindow);
		MonaApp.AddWindow(GameWindow, true);

		std::shared_ptr<FSceneViewport> SceneViewport = FSceneViewport::CreateWindowBacked(nullptr, GameWindow);
		SetMainSceneViewport(SceneViewport);
		const std::shared_ptr<FGenericWindow> NativeWindow = GameWindow->GetNativeWindow();
		SetGameInputWindow(NativeWindow);


		if (const FProjectInfo* CurrentProject = GetCurrentProject())
		{
			const FProjectGameSettingsStore SettingsStore = FProjectGameSettingsStore::ForProject(*CurrentProject);
			FProjectGameSettings Settings;
			const FProjectGameSettingsResult SettingsResult = SettingsStore.Load(Settings);
			if (!SettingsResult)
			{
				StartupError = SettingsResult.Message;
				DURIN_ERROR("Could not load project game settings: {}", StartupError);
				return true;
			}
			const FNativeGameModeResolution GameMode = ResolveNativeGameMode(Settings);
			if (!GameMode)
			{
				StartupError = GameMode.Result.Message;
				DURIN_ERROR("Could not resolve native gameplay bootstrap: {}", StartupError);
				return true;
			}
			if (!Settings.DefaultLevel.empty())
			{
				FPackagePath PackagePath;
				FObjectPath LevelPath;
				const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, PackagePath);
				if (!PathValidation)
					StartupError = std::format("Invalid Game.DefaultLevel '{}': {}", Settings.DefaultLevel, ToString(PathValidation.error()));
				else if (const auto Resolution = ResolveLevelPackage(PackagePath, LevelPath); !Resolution)
					StartupError = Resolution.Message;
				else
				{
					bStartupPending = OpenLevel({.Path = LevelPath, .GameMode = GameMode.GameModeClass});
					if (!bStartupPending) StartupError = "Could not start the default map change.";
				}
			}
		}
		if (!bStartupPending && StartupError.empty()) RequestGameMouseCapture(NativeWindow);
		return true;
	}
	auto DGameEngine::ShouldDrawStartupScene() const -> bool
	{
		const auto Progress = GetMapChangeProgress();
		return bStartupPending && Progress && Progress->Phase == EMapChangePhase::Committed;
	}
	auto DGameEngine::AdvanceStartup() -> void
	{
		if (bShutdownRequested) { bStartupPending = false; return; }
		if (!bStartupPending) return;
		FHostOperationScope Operation(*this);
		const auto Progress = GetMapChangeProgress();
		if (!Progress) return;
		if (Progress->Phase == EMapChangePhase::Committed)
		{
			const auto& Viewport = GetMainSceneViewport();
			if (Viewport->HasRenderFailed()) { StartupError = "The committed game scene could not render for presentation."; bStartupPending = false; return; }
			if (!Viewport->GetOutputReceipt()->IsPresented(GetSceneGeneration(), Viewport->GetRHIViewport().GetReference())) return;
			bStartupPending = false;
			if (const auto Window = GameInputWindow.lock()) RequestGameMouseCapture(Window);
		}
		else if (Progress->Phase == EMapChangePhase::Failed || Progress->Phase == EMapChangePhase::Canceled)
		{
			StartupError = Progress->Diagnostic;
			bStartupPending = false;
		}
	}
	auto DGameEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		if (bShutdownRequested) { bStartupPending = false; return Super::Tick(DeltaSeconds, bIdleMode); }
		if (GetStartupState() == EHostStartupState::Ready) Super::Tick(DeltaSeconds, bIdleMode);
	}

	auto DGameEngine::CloseSubsystemWork() -> void
	{
		bStartupPending = false;
		Super::CloseSubsystemWork();
	}

	auto DGameEngine::BeginDestroy() -> void
	{
		bStartupPending = false;
		ReleaseGameMouseCapture();
		DEngine::BeginDestroy();
	}

	auto DGameEngine::RequestGameMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool
	{
		if (!Window || !Window->IsFocused() || GetStartupState() != EHostStartupState::Ready) return false;
		if (GameInputWindow.lock() != Window) SetGameInputWindow(Window);
		if (CapturedMouseWindow.lock() == Window) return true;
		ReleaseGameMouseCapture();
		CapturedMouseWindow = Window;
		Window->SetCursorMode(ECursorMode::Captured);
		ResetGameInputMouse();
		SetGameInputEnabled(true);
		return true;
	}

	auto DGameEngine::ReleaseGameMouseCapture() -> void
	{
		if (const std::shared_ptr<FGenericWindow> Window = CapturedMouseWindow.lock())
		{
			Window->SetCursorMode(ECursorMode::Free);
		}
		CapturedMouseWindow.reset();
		SetGameInputEnabled(false);
	}

	auto DGameEngine::HandleGameInputWindowFocus(
		const std::shared_ptr<FGenericWindow>& Window,
		bool bFocused) -> void
	{
		if (!bFocused && GameInputWindow.lock() == Window) ReleaseGameMouseCapture();
	}

	auto DGameEngine::HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void
	{
		if (GameInputWindow.lock() == Window) ReleaseGameMouseCapture();
	}

	auto DGameEngine::HandleGameInputKeyDown(
		const std::shared_ptr<FGenericWindow>&,
		EKey Key,
		bool bRepeat) -> bool
	{
		if (Key != EKey::Escape || bRepeat || CapturedMouseWindow.expired()) return false;
		ReleaseGameMouseCapture();
		return true;
	}

	auto DGameEngine::HandleGameInputMouseDown(
		const std::shared_ptr<FGenericWindow>& Window,
		EMouseButton Button) -> bool
	{
		if (Button != EMouseButton::Left || !CapturedMouseWindow.expired()) return false;
		return RequestGameMouseCapture(Window);
	}
}
