#include "Engine/Engine.h"

#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectGlobals.h"
#include "IRendererModule.h"
#include "Mona/SceneViewport.h"
#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMesh.h"

#include "DynamicRHI.h"
#include "IScene.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	DEngine::~DEngine()
	{
		if (DemoStaticMeshActor)
		{
			if (DStaticMeshComponent* MeshComponent = DemoStaticMeshActor->GetStaticMeshComponent())
			{
				MeshComponent->UnregisterComponent();
				MeshComponent->SetStaticMesh(nullptr);
			}
		}
		DemoStaticMeshActor = nullptr;
		MainScene.reset();
		RendererModule = nullptr;
	}

	auto DEngine::Init() -> void
	{
		RendererModule = &FModuleManager::LoadModuleChecked<IRendererModule>("Renderer");
		MainScene = RendererModule->CreateScene();

		DemoStaticMeshActor = NewObject<AStaticMeshActor>(nullptr, "DebugStaticMeshActor");
		if (DStaticMeshComponent* MeshComponent = DemoStaticMeshActor->GetStaticMeshComponent())
		{
			MeshComponent->SetStaticMesh(DStaticMesh::CreateDebugTriangle());
			MeshComponent->RegisterComponent();
		}
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
				[RenderTargetRHI, Scene = MainScene.get(), RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					if (RendererModule != nullptr)
					{
						RendererModule->PrepareSceneResources(CommandList, Scene);
					}

					FRHIRenderPassInfo PassInfo{};
					PassInfo.ColorRenderTargets[0] = RenderTargetRHI;
					PassInfo.ColorClearValue = FClearValueBinding(0.05f, 0.09f, 0.14f, 1.0f);
					CommandList.BeginRenderPass(PassInfo, "StaticMeshRenderPass");
					if (RendererModule != nullptr)
					{
						RendererModule->RenderScene(CommandList, Scene, RenderTargetRHI, RenderTargetRHI->GetSizeX(), RenderTargetRHI->GetSizeY());
					}
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
			[ViewportRHI, Scene = MainScene.get(), RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
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
				if (RendererModule != nullptr)
				{
					RendererModule->PrepareSceneResources(CommandList, Scene);
				}
				CommandList.BeginRenderPass(PassInfo, "StaticMeshRenderPass");
				if (RendererModule != nullptr)
				{
					RendererModule->RenderScene(CommandList, Scene, BackBuffer, BackBuffer->GetSizeX(), BackBuffer->GetSizeY());
				}
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
