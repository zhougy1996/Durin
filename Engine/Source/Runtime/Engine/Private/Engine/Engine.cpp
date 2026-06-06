#include "Engine/Engine.h"

#include "Mona/SceneViewport.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto DEngine::Init() -> void
	{
	}

	auto DEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
	}

	auto DEngine::RedrawViewports() -> void
	{
		if (MainSceneViewport == nullptr)
		{
			return;
		}

		MainSceneViewport->UpdateRHIViewport();
		if (!MainSceneViewport->IsWindowBacked())
		{
			FTextureRHIRef RenderTargetRHI = MainSceneViewport->GetRenderTargetRHI();
			if (RenderTargetRHI == nullptr)
			{
				return;
			}

			ENQUEUE_RENDER_COMMAND(RenderMainSceneRenderTarget)(
				[RenderTargetRHI](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);

					FRHIRenderPassInfo PassInfo{};
					PassInfo.ColorRenderTargets[0] = RenderTargetRHI;
					PassInfo.ColorClearValue = FClearValueBinding(0.05f, 0.09f, 0.14f, 1.0f);
					CommandList.BeginRenderPass(PassInfo, "EditorSceneViewportClearPass");
					CommandList.EndRenderPass();
				}
			);
			return;
		}

		const TRefCountPtr<FRHIViewport>& ViewportRHI = MainSceneViewport->GetRHIViewport();
		if (ViewportRHI == nullptr)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(RenderMainSceneViewport)(
			[ViewportRHI](FRHICommandListImmediate& CommandList) {
				CommandList.SwitchPipeline(ERHIPipeline::Graphics);
				CommandList.BeginDrawingViewport(ViewportRHI, nullptr);

				FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
				if (BackBuffer == nullptr)
				{
					CommandList.EndDrawingViewport(ViewportRHI, false, false);
					return;
				}

				FRHIRenderPassInfo PassInfo{};
				PassInfo.ColorRenderTargets[0] = BackBuffer;
				PassInfo.ColorClearValue = FClearValueBinding(0.08f, 0.12f, 0.18f, 1.0f);
				CommandList.BeginRenderPass(PassInfo, "RuntimeSceneViewportClearPass");
				CommandList.EndRenderPass();

				CommandList.EndDrawingViewport(ViewportRHI, true, false);
			}
		);
	}

	auto DEngine::SetMainSceneViewport(std::shared_ptr<FSceneViewport> InSceneViewport) -> void
	{
		MainSceneViewport = std::move(InSceneViewport);
		if (MainSceneViewport)
		{
			MainSceneViewport->UpdateRHIViewport();
		}
	}

	DEngine* GEngine = nullptr;
} // namespace Durin
