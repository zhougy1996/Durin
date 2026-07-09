#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"
#include "IRendererModule.h"
#include "IScene.h"

namespace Durin
{
	class AStaticMeshActor;
	class ACameraActor;
	class DCameraComponent;
	class FSceneViewport;
	class IRendererModule;

	class ENGINE_API DEngine
	{
	public:
		virtual ~DEngine();

		virtual auto Init() -> void;

		virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;

		virtual auto RedrawViewports() -> void;

		virtual auto SetMainSceneViewport(std::shared_ptr<FSceneViewport> InSceneViewport) -> void;

		auto GetMainScene() const -> IScene* { return MainScene.get(); }
		auto GetRendererModule() const -> IRendererModule* { return RendererModule; }
		auto GetActiveCameraComponent() const -> DCameraComponent*;

	protected:
		auto BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView;

		IRendererModule* RendererModule = nullptr;
		std::unique_ptr<IScene> MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;
		TObjectPtr<AStaticMeshActor> DemoStaticMeshActor;
		TObjectPtr<ACameraActor> DefaultCameraActor;
	};

	extern ENGINE_API DEngine* GEngine;
}
