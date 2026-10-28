#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Input/GameInputState.h"
#include "SceneOwnership.h"
#include "Engine/World.h"
#include "Engine/EngineSubsystem.h"
#include "Engine/HostStartup.h"
#include "Engine/MapChange.h"
#include "DObject/ObjectLifecycle.h"

#include "Engine.gen.h"

namespace Durin
{
	class MWindow;

	// Narrow Launch-owned capability available while a concrete engine initializes.
	struct FEngineInitContext
	{
		// Launch creates the real primary native window before RHI device
		// admission and transfers the Mona-level owner here for viewport setup.
		std::shared_ptr<MWindow> StartupWindow;
		std::string ProjectAdmissionError;
	};

	class DCameraComponent;
	class FSceneViewport;
	class IRendererModule;
	class FRenderCommandFence;
	class FEngineInputEventHandler;
	class FGenericWindow;
	class DWorld;
	class FSceneInterface;
	class FMapChangeHandler;
	class FObjectPath;
	struct FSceneView;

	// Identifies a World's role in one engine; it does not own an editor or window.
	struct FWorldContext
	{
		EWorldType WorldType = EWorldType::Game;
		auto World() const -> DWorld* { return CurrentWorld.Get(); }
	private:
		TWeakObjectPtr<DWorld> CurrentWorld;
		friend class DEngine;
	};

	// Coordinates the active world, scene viewports, input state, and renderer module.
	DCLASS(NoClassDefaultObject)
	class DEngine : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DEngine(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DEngine() override;

		// Zero disables the foreground frame cap; lifecycle throttles remain independent.
		auto GetFrameRateLimit() const -> int32 { return FrameRateLimit; }
		auto SetFrameRateLimit(int32 FPS) -> void { FrameRateLimit = std::max(0, FPS); }

		virtual auto GetInitialWorldType() const -> EWorldType { return EWorldType::Game; }
		// Logs initialization errors locally; Launch owns cancellation and process teardown.
		ENGINE_API virtual auto Init(const FEngineInitContext& Context)
			-> bool;

		ENGINE_API virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;
		// Called once from the ordinary owner frame, outside simulation and UI callbacks.
		virtual auto AdvanceStartup() -> void {}
		virtual auto GetStartupState() const -> EHostStartupState { return EHostStartupState::Ready; }
		virtual auto ShouldDrawStartupScene() const -> bool { return false; }
		ENGINE_API virtual auto GetStartupError() const -> const std::string&;
		// Detaches host-owned consumers while the task system and objects are still alive.
		ENGINE_API virtual auto PrepareForShutdown() -> void;
		ENGINE_API virtual auto BeginTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject = nullptr) -> int32;
		ENGINE_API virtual auto EndTransaction() -> int32;
		ENGINE_API virtual auto CancelTransaction(int32 StartIndex = 0) -> void;
		virtual auto CanTransact() const -> bool { return false; }

		// External callbacks close admission immediately but retire consumers at the next host boundary.
		ENGINE_API auto DispatchSubsystemCallback(std::function<void()> Callback) -> void;
		ENGINE_API virtual auto RedrawViewports() -> void;

		ENGINE_API virtual auto SetMainSceneViewport(std::shared_ptr<FSceneViewport> InSceneViewport) -> void;
		ENGINE_API auto RegisterAuxiliarySceneViewport(const std::shared_ptr<FSceneViewport>& InSceneViewport) -> void;
		ENGINE_API auto UnregisterAuxiliarySceneViewport(const FSceneViewport* InSceneViewport) -> void;
		ENGINE_API virtual auto SetWorld(DWorld* InWorld) -> void;
		// A second pending change or closed host admission is rejected.
		ENGINE_API auto PrepareMapChange(FMapChangeRequest Request) -> bool;
		ENGINE_API auto IsMapChangeReady() const -> bool;
		// Requests activation at a safe frame boundary once ready. OnComplete
		// reports the final result; true only means the request was accepted.
		ENGINE_API auto CommitMapChange() -> bool;
		ENGINE_API auto CancelPendingMapChange() -> void;
		// Ordinary opening prepares and requests commit without caller polling.
		ENGINE_API auto OpenLevel(FMapChangeRequest Request) -> bool;
		ENGINE_API auto OpenLevel(const FObjectPath& Path) -> bool;
		ENGINE_API auto IsPreparingMapChange() const -> bool;
		ENGINE_API auto GetMapChangeProgress() const -> std::optional<FMapChangeProgress>;
		auto GetSceneGeneration() const -> uint64 { return SceneGeneration; }
		ENGINE_API auto GetWorldContextFromWorld(const DWorld* World) const -> const FWorldContext*;
		auto GetWorldContexts() const -> std::span<const FWorldContext> { return WorldContexts; }

		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto FinishDestroy() -> void override;

		template<typename T> requires std::is_base_of_v<DEngineSubsystem, T>
		auto GetSubsystem() const -> T* { return static_cast<T*>(EngineSubsystems.Find(T::StaticClass())); }
		auto GetSubsystemState() const -> ESubsystemState { return EngineSubsystems.GetState(); }
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;

		auto GetMainScene() const -> FSceneInterface* { return MainScene.get(); }
		auto GetMainSceneViewport() const -> const std::shared_ptr<FSceneViewport>& { return MainSceneViewport; }
		auto GetRendererModule() const -> IRendererModule* { return RendererModule; }
		ENGINE_API auto GetActiveCameraComponent() const -> DCameraComponent*;
		auto GetWorld() const -> DWorld* { return MainWorld.Get(); }
		auto GetGameInputState() const -> const FGameInputState& { return GameInputState; }
		ENGINE_API auto SetGameInputEnabled(bool bEnabled) -> void;
		ENGINE_API auto SetGameInputWindow(const std::shared_ptr<FGenericWindow>& InWindow) -> void;
		ENGINE_API auto ClearGameInputWindow() -> void;
		ENGINE_API auto ResetGameInputMouse() -> void;

	protected:
		ENGINE_API auto BuildWorldSceneView(const DWorld& World, uint32 Width, uint32 Height) const -> FSceneView;
		ENGINE_API virtual auto AdoptSceneWorld(DWorld* World, FScenePtr Scene, uint64 Generation) -> void;
		// Content admission runs after host services exist and schema providers load.
		ENGINE_API auto InitializeProjectContent() -> std::expected<void, std::string>;
		// Pins host lifecycle mutation until a complete host callback has returned.
		class FHostOperationScope
		{
		public:
			ENGINE_API explicit FHostOperationScope(DEngine& InHost, bool bFlushOnExit = true);
			ENGINE_API ~FHostOperationScope();
		private:
			FGarbageCollectionDeferralScope Deferral;
			DEngine& Host;
			bool bFlushOnExit;
		};
		ENGINE_API virtual auto CloseSubsystemWork() -> void;
		virtual auto RetireHostConsumers() -> void {}
		ENGINE_API virtual auto AreHostConsumersIdle() -> bool;
		auto IsHostOperationIdle() -> bool { return HostOperationDepth == 0 && !bPreparingShutdown && AreHostConsumersIdle(); }
		ENGINE_API auto InitializeEngineSubsystems() -> FSubsystemResult;
		FEngineSubsystemCollection EngineSubsystems;
		bool bShutdownRequested = false;
		bool bShutdownComplete = false;
	private:
		friend class FEngineLoop;
		friend struct FMapChangeTestAccess;
		friend class FMapChangeHandler;
		ENGINE_API auto TickMapChange() -> void;
		// Tracks unpublished Worlds so host retirement can close their admission during boot.
		class FWorldInitializationScope
		{
		public:
			ENGINE_API FWorldInitializationScope(DEngine& InHost, DWorld& InWorld);
			ENGINE_API ~FWorldInitializationScope();
		private:
			FHostOperationScope Operation;
			DEngine& Host;
			DWorld& World;
		};
		std::vector<DWorld*> InitializingWorlds;
		std::vector<FWorldContext> WorldContexts;
		std::unique_ptr<FMapChangeHandler> MapChange;
		std::optional<FMapChangeProgress> LastMapChangeProgress;
		uint64 SceneGeneration = 0;
		// World lifecycle and SetWorld are the only writers of the context registry.
		ENGINE_API auto RegisterWorldContext(DWorld& World) -> void;
		ENGINE_API auto DestroyWorldContext(DWorld* World) -> void;
		friend class DWorld;
		auto InitInternal(const FEngineInitContext& Context) -> bool;
		int32 FrameRateLimit = 0;
		uint32 HostOperationDepth = 0;
		bool bPreparingShutdown = false;
		bool bInitStarted = false;
	protected:
		ENGINE_API auto BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView;
		ENGINE_API auto BuildSceneView(const FSceneViewport* SceneViewport, uint32 Width, uint32 Height, bool bAllowCameraFallback, FSceneView& OutView) const -> bool;
		virtual auto HandleGameInputWindowFocus(const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void {}
		virtual auto HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void {}
		virtual auto HandleGameInputKeyDown(const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat) -> bool { return false; }
		virtual auto HandleGameInputMouseDown(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool { return false; }

		IRendererModule* RendererModule = nullptr;
		FScenePtr MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;
		std::vector<std::shared_ptr<FSceneViewport>> AuxiliarySceneViewports;
		std::unique_ptr<FRenderCommandFence> DestroyFence;

		// Active world is retained by GC while the engine owns it.
		DPROPERTY(Transient)
		TObjectPtr<DWorld> MainWorld;
		FGameInputState GameInputState;
		std::weak_ptr<FGenericWindow> GameInputWindow;
		mutable const DCameraComponent* LastCameraViewSource = nullptr;
		mutable bool bHasCameraViewSource = false;

		friend class FEngineInputEventHandler;
		friend struct FEngineInputTestAccess;
	};

	extern ENGINE_API DEngine* GEngine;
} // namespace Durin
