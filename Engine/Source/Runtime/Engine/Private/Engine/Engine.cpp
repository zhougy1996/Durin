#include "Engine/Engine.h"

#include "Mona/SceneViewport.h"

#include "Application/MonaApplication.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	auto DEngine::Init() -> void
	{
		auto& App = Mona::FMonaApplication::Get();
		const auto& Windows = App.GetWindows();
		if (Windows.empty())
		{
			return;
		}

		MainSceneViewport = std::make_shared<FSceneViewport>(nullptr, Windows.front());
		Windows.front()->SetViewport(MainSceneViewport);
		MainSceneViewport->UpdateRHIViewport();
	}

	auto DEngine::Start() -> void
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

	DEngine* GEngine = nullptr;
} // namespace Durin
