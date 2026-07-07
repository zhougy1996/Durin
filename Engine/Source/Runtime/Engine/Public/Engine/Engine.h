#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class AStaticMeshActor;
	class FSceneViewport;
	class IScene;
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

	protected:
		IRendererModule* RendererModule = nullptr;
		std::unique_ptr<IScene> MainScene;
		std::shared_ptr<FSceneViewport> MainSceneViewport;
		TObjectPtr<AStaticMeshActor> DemoStaticMeshActor;
	};

	extern ENGINE_API DEngine* GEngine;
}
