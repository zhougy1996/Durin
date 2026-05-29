#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class FSceneViewport;

	class ENGINE_API DEngine
	{
	public:
		virtual auto Init() -> void;

		virtual auto Start() -> void;

		virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;

		virtual auto RedrawViewports() -> void;

	protected:
		std::shared_ptr<FSceneViewport> MainSceneViewport;
	};

	extern ENGINE_API DEngine* GEngine;
}
