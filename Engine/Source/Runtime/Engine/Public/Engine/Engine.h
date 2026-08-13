#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Input/GameInputState.h"

#include "Engine.gen.h"

namespace Durin
{
	enum class EEngineInitializationStatus : uint8
	{
		Succeeded,
		Cancelled,
		Failed,
	};

	struct FEngineInitializationResult
	{
		EEngineInitializationStatus Status = EEngineInitializationStatus::Succeeded;
		std::string Message;

		explicit operator bool() const
		{
			return Status == EEngineInitializationStatus::Succeeded;
		}

		static auto Success() -> FEngineInitializationResult { return {}; }
		static auto Cancelled(std::string InMessage = {}) -> FEngineInitializationResult
		{
			return {EEngineInitializationStatus::Cancelled, std::move(InMessage)};
		}
		static auto Failure(std::string InMessage) -> FEngineInitializationResult
		{
			return {EEngineInitializationStatus::Failed, std::move(InMessage)};
		}
	};

	// Narrow Launch-owned capability available while a concrete engine initializes.
	struct FEngineInitContext
	{
		std::function<bool()> PumpStartupFrame;
		bool bHeadless = false;
	};

	class DCameraComponent;
	class FSceneViewport;
	class IRendererModule;
	class FRenderCommandFence;
	class FEngineInputEventHandler;
	class FGenericWindow;
	class DWorld;
	class IScene;
	struct FSceneView;

	// Coordinates the active world, scene viewports, input state, and renderer module.
	DCLASS(NoClassDefaultObject)
	class DEngine : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DEngine(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DEngine() override;

		ENGINE_API virtual auto Init(const FEngineInitContext& Context)
			-> FEngineInitializationResult;

		ENGINE_API virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;
		// Detaches host-owned consumers while the task system and objects are still alive.
		ENGINE_API virtual auto PrepareForShutdown() -> void;

		ENGINE_API virtual auto RedrawViewports() -> void;

		ENGINE_API virtual auto SetMainSceneViewport(std::shared_ptr<FSceneViewport> InSceneViewport) -> void;
		ENGINE_API auto RegisterAuxiliarySceneViewport(const std::shared_ptr<FSceneViewport>& InSceneViewport) -> void;
		ENGINE_API auto UnregisterAuxiliarySceneViewport(const FSceneViewport* InSceneViewport) -> void;
		ENGINE_API virtual auto SetWorld(DWorld* InWorld) -> void;

		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto FinishDestroy() -> void override;

		auto GetMainScene() const -> IScene* { return MainScene.get(); }
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
		ENGINE_API auto BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView;
		ENGINE_API auto BuildSceneView(const FSceneViewport* SceneViewport, uint32 Width, uint32 Height, bool bAllowCameraFallback, FSceneView& OutView) const -> bool;
		virtual auto HandleGameInputWindowFocus(const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void {}
		virtual auto HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void {}
		virtual auto HandleGameInputKeyDown(const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat) -> bool { return false; }
		virtual auto HandleGameInputMouseDown(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool { return false; }

		IRendererModule* RendererModule = nullptr;
		std::unique_ptr<IScene> MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;
		std::vector<std::shared_ptr<FSceneViewport>> AuxiliarySceneViewports;
		std::unique_ptr<FRenderCommandFence> DestroyFence;

		// Active world is retained by GC while the engine owns it.
		DPROPERTY(Transient)
		TObjectPtr<DWorld> MainWorld;
		FGameInputState GameInputState;
		std::weak_ptr<FGenericWindow> GameInputWindow;

		friend class FEngineInputEventHandler;
		friend struct FEngineInputTestAccess;
	};

	extern ENGINE_API DEngine* GEngine;
} // namespace Durin
