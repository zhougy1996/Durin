#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Input/GameInputState.h"

#include "Engine.gen.h"

namespace Durin
{
	class DCameraComponent;
	class FSceneViewport;
	class IRendererModule;
	class FRenderCommandFence;
	class FEngineInputEventHandler;
	class DWorld;
	class IScene;
	struct FSceneView;

	// Coordinates the active world, scene viewports, input state, and renderer module.
	DCLASS()
	class DEngine : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DEngine(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DEngine() override;

		ENGINE_API virtual auto Init() -> void;

		ENGINE_API virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;

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

	protected:
		ENGINE_API auto BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView;
		ENGINE_API auto BuildSceneView(const FSceneViewport* SceneViewport, uint32 Width, uint32 Height, bool bAllowCameraFallback, FSceneView& OutView) const -> bool;

		IRendererModule* RendererModule = nullptr;
		std::unique_ptr<IScene> MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;
		std::vector<std::shared_ptr<FSceneViewport>> AuxiliarySceneViewports;
		std::unique_ptr<FRenderCommandFence> DestroyFence;

		// Active world is retained by GC while the engine owns it.
		DPROPERTY(Transient)
		TObjectPtr<DWorld> MainWorld;
		FGameInputState GameInputState;

		friend class FEngineInputEventHandler;
	};

	extern ENGINE_API DEngine* GEngine;
} // namespace Durin
