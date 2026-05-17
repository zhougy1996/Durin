#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class ENGINE_API DEngine
	{
	public:
		virtual auto Init() -> void;

		virtual auto Start() -> void;

		virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;

		virtual auto RedrawViewports() -> void {};
	};

	extern ENGINE_API DEngine* GEngine;
}