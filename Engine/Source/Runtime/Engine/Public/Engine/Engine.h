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
	class FEngineInputEventHandler;
	class DWorld;
	class IScene;
	struct FSceneView;

	DCLASS()
	class ENGINE_API DEngine : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DEngine(const FObjectInitializer& ObjectInitializer);
		~DEngine() override;

		virtual auto Init() -> void;

		virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;

		virtual auto RedrawViewports() -> void;

		virtual auto SetMainSceneViewport(std::shared_ptr<FSceneViewport> InSceneViewport) -> void;
		auto RegisterAuxiliarySceneViewport(const std::shared_ptr<FSceneViewport>& InSceneViewport) -> void;
		auto UnregisterAuxiliarySceneViewport(const FSceneViewport* InSceneViewport) -> void;
		virtual auto SetWorld(DWorld* InWorld) -> void;

		auto BeginDestroy() -> void override;

		auto GetMainScene() const -> IScene* { return MainScene.get(); }
		auto GetMainSceneViewport() const -> const std::shared_ptr<FSceneViewport>& { return MainSceneViewport; }
		auto GetRendererModule() const -> IRendererModule* { return RendererModule; }
		auto GetActiveCameraComponent() const -> DCameraComponent*;
		auto GetWorld() const -> DWorld* { return MainWorld.Get(); }
		auto GetGameInputState() const -> const FGameInputState& { return GameInputState; }
		auto SetGameInputEnabled(bool bEnabled) -> void { GameInputState.SetEnabled(bEnabled); }

	protected:
		auto BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView;
		auto BuildSceneView(const FSceneViewport* SceneViewport, uint32 Width, uint32 Height, bool bAllowCameraFallback, FSceneView& OutView) const -> bool;

		IRendererModule* RendererModule = nullptr;
		std::unique_ptr<IScene> MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;
		std::vector<std::shared_ptr<FSceneViewport>> AuxiliarySceneViewports;

		DPROPERTY(Transient)
		TObjectPtr<DWorld> MainWorld;
		FGameInputState GameInputState;

		friend class FEngineInputEventHandler;
	};

	extern ENGINE_API DEngine* GEngine;
} // namespace Durin
