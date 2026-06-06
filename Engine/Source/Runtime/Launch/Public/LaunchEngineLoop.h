#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class FEngineLoop
	{
	public:
		auto PreInit() -> void;
		auto Init() -> void;
		auto Tick() -> void;
		auto Exit() -> void;

	private:
		auto SubmitRenderFrame(
			uint64 LogicFrameCounter,
			uint64 RenderFrameCounter,
			bool bRenderSceneViewports,
			const std::function<void()>& QueueUiRenderWork
		) -> bool;

		bool bIsSubmittingRenderFrame = false;
	};

	extern FEngineLoop GEngineLoop;
}
