#include "Tickable.h"
#include "Engine/GameEngine.h"
#include "Engine/AssetManager.h"
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

	auto DGameEngine::Init(const FEngineInitContext& Context) -> bool
	{

		if (!DEngine::Init(Context))
		{
			if (bShutdownRequested) FailStartup("Game engine services could not initialize.");
			return false;
		}
		StartupPhase = EStartupPhase::StartProject;
		StartupError = Context.ProjectAdmissionError;
		auto FailInitialization = [&](std::string Error) {
			FailStartup(std::move(Error));
			RequestShutdown();
			return false;
		};
		const auto& GameWindow = Context.StartupWindow;
		if (!GameWindow || !GameWindow->GetNativeWindow())
			return FailInitialization("Game initialization requires a native startup window.");
		if (!Mona::FMonaApplication::IsInitialized() || !Mona::FMonaApplication::Get().GetRenderer())
			return FailInitialization("Game initialization requires Mona window presentation services.");
		auto& MonaApp = Mona::FMonaApplication::Get();
		MonaApp.GetRenderer()->CreateViewport(GameWindow);
		MonaApp.AddWindow(GameWindow, true);
		SetMainSceneViewport(FSceneViewport::CreateWindowBacked(nullptr, GameWindow));
		SetGameInputWindow(GameWindow->GetNativeWindow());
		if (bShutdownRequested || IsEngineExitRequested())
		{
			RequestShutdown();
			return false;
		}
		return true;
	}

	auto DGameEngine::StartGame() -> std::expected<void, std::string>
	{
		if (!StartupError.empty()) return std::unexpected(StartupError);
		FPackagePath DefaultLevel;
		DClass* GameModeClass = nullptr;
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const auto Modules = Project->EnabledRootModules;
			for (const auto& ModuleName : Modules)
			{
				if (!FModuleManager::Get().LoadModule(FName(ModuleName)))
					return std::unexpected(std::format("Game startup could not load project module '{}'.", ModuleName));
				if (bShutdownRequested || IsEngineExitRequested()) return std::unexpected("Game startup was canceled.");
			}
			FProjectGameSettings Settings;
			if (const auto Result = FProjectGameSettingsStore::ForProject(*Project).Load(Settings); !Result)
				return std::unexpected(Result.Message);
			const auto GameMode = ResolveNativeGameMode(Settings);
			if (!GameMode) return std::unexpected(GameMode.Result.Message);
			GameModeClass = GameMode.GameModeClass;
			if (!Settings.DefaultLevel.empty())
				if (const auto Validation = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, DefaultLevel); !Validation)
					return std::unexpected(std::format("Invalid Game.DefaultLevel '{}': {}", Settings.DefaultLevel, ToString(Validation.error())));
		}
		if (bShutdownRequested || IsEngineExitRequested()) return std::unexpected("Game startup was canceled.");
		if (const auto Content = InitializeProjectContent(); !Content) return std::unexpected(Content.error());
		if (bShutdownRequested || IsEngineExitRequested()) return std::unexpected("Game startup was canceled.");
		if (DefaultLevel.IsValid())
		{
			FSoftObjectPath LevelPath;
			if (const auto Resolution = ResolveLevelPackage(DefaultLevel, LevelPath); !Resolution)
				return std::unexpected(Resolution.Message);
			auto* Manager = GetAssetManager();
			const auto PrimaryId = Manager ? Manager->GetPrimaryAssetIdForPath(LevelPath.GetAssetPath()) : FPrimaryAssetId{};
			const auto Preload = PrimaryId.IsValid() ? Manager->PreloadPrimaryAssets(std::span(&PrimaryId, 1)) : nullptr;
			if (!OpenLevel({.Path = LevelPath, .Preload = Preload, .GameMode = GameModeClass}))
				return std::unexpected("Could not start the default map change.");
		}
		return {};
	}

	auto DGameEngine::FailStartup(std::string Error) -> void
	{
		StartupError = Error.empty() ? "Game startup failed without a diagnostic." : std::move(Error);
		StartupPhase = EStartupPhase::Failed;
		DURIN_ERROR("Game startup failed: {}", StartupError);
	}

	auto DGameEngine::CompleteStartup() -> void
	{
		if (bShutdownRequested || IsEngineExitRequested()) return;
		StartupPhase = EStartupPhase::Ready;
		StartupError.clear();
		if (const auto Window = GameInputWindow.lock()) RequestGameMouseCapture(Window);
	}

	auto DGameEngine::ShouldDrawStartupScene() const -> bool
	{
		const auto Progress = GetMapChangeProgress();
		return StartupPhase == EStartupPhase::AwaitingScene && Progress && Progress->Phase == EMapChangePhase::Committed;
	}

	auto DGameEngine::AdvanceStartup() -> void
	{
		if (bShutdownRequested || IsEngineExitRequested() || GetStartupState() != EHostStartupState::Pending) return;

		if (StartupPhase == EStartupPhase::StartProject)
		{
			auto Started = StartGame();
			if (bShutdownRequested || IsEngineExitRequested()) return;
			if (!Started) { FailStartup(std::move(Started.error())); return; }
			if (IsPreparingMapChange()) StartupPhase = EStartupPhase::AwaitingScene;
			else CompleteStartup();
			return;
		}
		const auto Progress = GetMapChangeProgress();
		if (!Progress) { FailStartup("The default map change is no longer available."); return; }
		if (Progress->Phase == EMapChangePhase::Committed)
		{
			const auto& Viewport = GetMainSceneViewport();
			if (!Viewport) { FailStartup("The game startup scene viewport is no longer available."); return; }
			const auto Presentation = Viewport->QueryPresentation(GetSceneGeneration());
			if (Presentation == EScenePresentationState::Failed) { FailStartup("The committed game scene could not render for presentation."); return; }
			if (Presentation == EScenePresentationState::Presented) CompleteStartup();
		}
		else if (Progress->Phase == EMapChangePhase::Failed || Progress->Phase == EMapChangePhase::Canceled)
			FailStartup(Progress->Diagnostic);
	}
	auto DGameEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		if (bShutdownRequested || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f
			|| GetStartupState() != EHostStartupState::Ready) return;
		Super::Tick(DeltaSeconds, bIdleMode);
		if (!bShutdownRequested) FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, DeltaSeconds);
	}

	auto DGameEngine::RequestSubsystemDeinitialization() -> void
	{
		ReleaseGameMouseCapture();
		Super::RequestSubsystemDeinitialization();
	}

	auto DGameEngine::BeginDestroy() -> void
	{
		ReleaseGameMouseCapture();
		DEngine::BeginDestroy();
	}

	auto DGameEngine::RequestGameMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool
	{
		if (bShutdownRequested || IsEngineExitRequested() || !Window || !Window->IsFocused() || GetStartupState() != EHostStartupState::Ready) return false;
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
