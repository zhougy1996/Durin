#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "Engine.gen.h"

namespace Durin
{
	class AStaticMeshActor;
	class ACameraActor;
	class DCameraComponent;
	class FSceneViewport;
	class IRendererModule;
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

		auto BeginDestroy() -> void override;

		auto GetMainScene() const -> IScene* { return MainScene.get(); }
		auto GetRendererModule() const -> IRendererModule* { return RendererModule; }
		auto GetActiveCameraComponent() const -> DCameraComponent*;
		auto GetWorld() const -> DWorld* { return MainWorld.Get(); }

	protected:
		auto BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView;

		IRendererModule* RendererModule = nullptr;
		std::unique_ptr<IScene> MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;

		DPROPERTY()
		TObjectPtr<DWorld> MainWorld;

		DPROPERTY()
		TObjectPtr<AStaticMeshActor> DemoStaticMeshActor;

		DPROPERTY()
		TObjectPtr<ACameraActor> DefaultCameraActor;
	};

	extern ENGINE_API DEngine* GEngine;
} // namespace Durin
