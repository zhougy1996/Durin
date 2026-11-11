#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "IRendererModule.h"
#include "RHICommandList.h"

namespace Durin::Tests
{
	// The caller owns the render thread. Each preparation attempt gets its own
	// frame and admission budget before the measured draw begins.
	template<typename TPrepare>
	auto WaitForQualificationResources(FRHICommandListImmediate& Commands, TPrepare Prepare) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		do
		{
			++GRenderFrameCounterRenderThread;
			GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
			const auto State = Prepare();
			GDynamicRHI->RHIEndFrame_RenderThread(Commands);
			if (State != ERenderResourcePreparationState::Pending)
				return State == ERenderResourcePreparationState::Ready;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < Deadline);
		return false;
	}

	inline auto PrepareQualificationView(IRendererModule& Renderer, FRHICommandListImmediate& Commands,
		const FSceneViewRenderRequest& Request) -> bool
	{
		const auto Preparation = FRenderResourcePreparation::Create();
		return WaitForQualificationResources(Commands, [&] {
			Renderer.BeginFrame_RenderThread(Commands);
			return Renderer.PrepareViewResources(Commands, Request, Preparation).State;
		});
	}
}
