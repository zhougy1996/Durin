#include "Engine/GameEngine.h"

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
	struct FGameStartupPreparation
	{
		TStrongObjectPtr<DLevel> Level;
		DClass* GameMode = nullptr;
		std::vector<FWeakObjectPtr> Meshes, ActiveMeshes;
		size_t NextMesh = 0;
		bool MeshesCollected = false;
		std::vector<FWeakObjectPtr> Materials;
		std::vector<FMaterialProgramPreparation> Active;
		size_t Next = 0;
		bool Collected = false;
	};

	// Request initial geometry before registration; an empty manager alone does
	// not mean metadata-only meshes have become resident.
	static auto PumpStartupGeometry(FGameStartupPreparation& Loading) -> std::expected<bool, std::string>
	{
		if (!Loading.MeshesCollected)
		{
			std::unordered_set<DStaticMesh*> Seen;
			for (const auto& Actor : Loading.Level->GetActors())
				if (IsValid(Actor.Get()))
					for (const auto* Component : Actor->FindComponentsByClass<DMeshComponent>())
					{
						auto* Mesh = [&]() -> DStaticMesh* {
							if (const auto* Static = Cast<DStaticMeshComponent>(Component)) return Static->GetStaticMesh();
							if (const auto* Spline = Cast<DSplineMeshComponent>(Component)) return Spline->GetStaticMesh();
							return nullptr;
						}();
						if (IsValid(Mesh) && Seen.insert(Mesh).second) Loading.Meshes.emplace_back(Mesh);
					}
			Loading.MeshesCollected = true;
			if (Loading.Meshes.size() > 4096) return std::unexpected("Default level geometry preparation exceeds 4096 owners.");
		}
		bool Failed = false;
		std::erase_if(Loading.ActiveMeshes, [&](const auto& Owner) {
			auto* Mesh = Cast<DStaticMesh>(Owner.Get());
			if (!IsValid(Mesh)) { Failed = true; return true; }
			const auto Status = Mesh->GetRenderDataLoadStatus();
			if (Status.CpuPhase == ECookedMeshCpuPhase::Failed || Status.CpuPhase == ECookedMeshCpuPhase::Cancelled
				|| Status.GpuPhase == ECookedMeshGpuPhase::Failed) { Failed = true; return true; }
			return Status.CpuPhase == ECookedMeshCpuPhase::CpuReady && Status.GpuPhase == ECookedMeshGpuPhase::Ready;
		});
		if (Failed) return std::unexpected("Default level geometry preparation failed or was unloaded.");
		while (Loading.ActiveMeshes.size() < 4 && Loading.NextMesh < Loading.Meshes.size())
		{
			if (const auto* Manager = GetCookedMeshLoadManager())
			{
				const auto Pending = Manager->GetDiagnostics();
				if (Pending.InFlightCount + Pending.PendingRequestCount >= 8) break;
			}
			auto* Mesh = Cast<DStaticMesh>(Loading.Meshes[Loading.NextMesh++].Get());
			if (!IsValid(Mesh)) return std::unexpected("Default level geometry was unloaded during preparation.");
			(void)Mesh->RequestRenderDataAndResources();
			Loading.ActiveMeshes.emplace_back(Mesh);
		}
		return Loading.NextMesh == Loading.Meshes.size() && Loading.ActiveMeshes.empty();
	}

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
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			for (const std::string& ModuleName : Project->EnabledRootModules)
				if (!FModuleManager::Get().LoadModule(FName(ModuleName)))
				{
					DURIN_ERROR("Game initialization could not load project module '{}'.", ModuleName);
					return false;
				}
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
		RequestGameMouseCapture(NativeWindow);

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
				DLevel* Level = nullptr;
				const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, PackagePath);
				if (PathValidation)
				{
					auto Result =
						ResolveLevelPackage(PackagePath, LevelPath);
					if (Result)
					{
						auto Loaded = LoadObject<DLevel>(LevelPath);
						if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
						else Level = *Loaded;
					}
					if (Result && GetAssetRuntimeConfiguration().RequiresCookedPayload())
					{
						StartupPreparation = std::make_shared<FGameStartupPreparation>();
						StartupPreparation->Level = Level;
						StartupPreparation->GameMode = GameMode.GameModeClass;
						return true;
					}
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
					StartupError = std::format(
						"Project Game.DefaultLevel '{}' is not a valid package path: {}",
						Settings.DefaultLevel, ToString(PathValidation.error()));
					DURIN_WARN("{}", StartupError);
				}
			}
		}
		return true;
	}

	auto DGameEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		if (bShutdownRequested) { StartupPreparation.reset(); return Super::Tick(DeltaSeconds, bIdleMode); }
		if (StartupPreparation)
		{
			FHostOperationScope Operation(*this);
			const auto Pending = StartupPreparation;
			PumpCookedMeshLoadManager();
			if (bShutdownRequested) return;
			auto& Loading = *Pending;
			const auto Geometry = PumpStartupGeometry(Loading);
			if (!Geometry) { StartupError = Geometry.error(); StartupPreparation.reset(); DURIN_ERROR("{}", StartupError); return; }
			if (!*Geometry) return;
			if (!Loading.Collected)
			{
				std::unordered_set<DMaterialInterface*> Seen;
				for (const auto& Actor : Loading.Level->GetActors())
					if (IsValid(Actor.Get()))
						for (const auto* Mesh : Actor->FindComponentsByClass<DMeshComponent>())
							for (uint32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
								for (auto* Material = Mesh->GetMaterial(Slot); IsValid(Material) && Seen.insert(Material).second; Material = Material->GetParent())
									if (!Material->IsDynamicInstance()) Loading.Materials.emplace_back(Material);
				Loading.Collected = true;
				if (Loading.Materials.size() > 4096)
				{
					StartupError = "Default level material preparation exceeds 4096 owners.";
					StartupPreparation.reset(); return;
				}
			}
			bool Failed = false;
			std::erase_if(Loading.Active, [&](auto& Program) {
				const auto State = Program.Poll();
				if (State == EMaterialProgramPreparationState::Pending) return false;
				if (State != EMaterialProgramPreparationState::Ready)
				{
					Failed = true;
					StartupError = "Default level material preparation failed: " + Program.GetDiagnostic();
				}
				return true;
			});
			if (bShutdownRequested) { StartupPreparation.reset(); return; }
			if (Failed) { StartupPreparation.reset(); DURIN_ERROR("{}", StartupError); return; }
			while (Loading.Active.size() < 8 && Loading.Next < Loading.Materials.size())
			{
				auto* Material = Cast<DMaterialInterface>(Loading.Materials[Loading.Next++].Get());
				if (!IsValid(Material)) { StartupError = "Default level material was unloaded during preparation."; StartupPreparation.reset(); return; }
				Loading.Active.push_back(FMaterialProgramPreparation::Request(*Material));
			}
			if (!Loading.Active.empty() || Loading.Next < Loading.Materials.size()) return;
			DURIN_DEBUG("Cooked startup material programs prepared: {}", Loading.Materials.size());
			auto Level = Loading.Level;
			auto* GameMode = Loading.GameMode;
			StartupPreparation.reset();
			if (!GetWorld()->SetCurrentLevel(Level.Get())) { StartupError = "Prepared default level could not activate."; return; }
			if (GetMainSceneViewport()) GetMainSceneViewport()->RequestResourcePreparation();
			const auto Play = GetWorld()->BeginPlay({.GameModeClass = GameMode});
			if (!Play) { StartupError = Play.Message; DURIN_ERROR("{}", StartupError); return; }
		}
		Super::Tick(DeltaSeconds, bIdleMode);
	}

	auto DGameEngine::CloseSubsystemWork() -> void
	{
		StartupPreparation.reset();
		Super::CloseSubsystemWork();
	}

	auto DGameEngine::BeginDestroy() -> void
	{
		StartupPreparation.reset();
		ReleaseGameMouseCapture();
		DEngine::BeginDestroy();
	}

	auto DGameEngine::RequestGameMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool
	{
		if (!Window || !Window->IsFocused()) return false;
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
