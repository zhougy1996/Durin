#pragma once

namespace Doge
{
	class ENGINE_API DEngine
	{
		virtual auto Init() -> void;

		virtual auto Start() -> void;

		virtual auto Tick(float DeltaSeconds, bool bIdleMode) -> void;

		virtual auto RedrawViewports() -> void {};
	};
}