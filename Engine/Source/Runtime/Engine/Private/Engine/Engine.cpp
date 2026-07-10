#include "Engine/Engine.h"
#include "Engine/World.h"

#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectGlobals.h"
#include "IRendererModule.h"
#include "Misc/Paths.h"
#include "Mona/SceneViewport.h"
#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMesh.h"

#include "DynamicRHI.h"
#include "IScene.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	DEngine::DEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	DEngine::~DEngine() = default;

	auto DEngine::Init() -> void
	{
		RendererModule = &FModuleManager::LoadModuleChecked<IRendererModule>("Renderer");
		MainScene = RendererModule->CreateScene();
		MainWorld = NewObject<DWorld>(this, "MainWorld");

		DefaultCameraActor = MainWorld->SpawnActor<ACameraActor>("DefaultCameraActor");
		if (DCameraComponent* CameraComponent = DefaultCameraActor->GetCameraComponent())
		{
			CameraComponent->SetLookAt(FVector3(-3.0, -6.0, 2.25), FVector3(0.0, 0.0, 0.0));
		}

		DemoStaticMeshActor = MainWorld->SpawnActor<AStaticMeshActor>("DebugStaticMeshActor");
		if (DStaticMeshComponent* MeshComponent = DemoStaticMeshActor->GetStaticMeshComponent())
		{
			const std::string TeapotPath = FPaths::EngineDir() + "Content/Test/teapot.obj";
			std::shared_ptr<DStaticMesh> TeapotMesh = DStaticMesh::CreateFromFile(TeapotPath);
			if (TeapotMesh == nullptr)
			{
				DURIN_ERROR("Failed to initialize demo static mesh actor with teapot: {}", TeapotPath);
				return;
			}

			MeshComponent->SetWorldLocation(FVector3(0.0, 0.0, 0.0));
			MeshComponent->SetStaticMesh(std::move(TeapotMesh));
			MeshComponent->RegisterComponent();
		}
	}

	auto DEngine::BeginDestroy() -> void
	{
		MainSceneViewport.reset();
		DemoStaticMeshActor = nullptr;
		DefaultCameraActor = nullptr;
		MainWorld = nullptr;
		MainScene.reset();
		RendererModule = nullptr;
		Super::BeginDestroy();
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

			const FSceneView View = BuildMainSceneView(RenderTargetRHI->GetSizeX(), RenderTargetRHI->GetSizeY());
			ENQUEUE_RENDER_COMMAND(RenderMainSceneRenderTarget)(
				[RenderTargetRHI, View, Scene = MainScene.get(), RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					if (RendererModule != nullptr)
					{
						RendererModule->PrepareSceneResources(CommandList, Scene);
					}

					if (RendererModule != nullptr)
					{
						RendererModule->RenderView(CommandList, Scene, View, RenderTargetRHI, false);
					}
				}
			);
			return;
		}

		const TRefCountPtr<FRHIViewport>& ViewportRHI = MainSceneViewport->GetRHIViewport();
		if (ViewportRHI == nullptr)
		{
			return;
		}

		const FVector2f DesiredSize = MainSceneViewport->GetDesiredSize();
		const uint32 ViewWidth = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.x)));
		const uint32 ViewHeight = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.y)));
		const FSceneView View = BuildMainSceneView(ViewWidth, ViewHeight);
		ENQUEUE_RENDER_COMMAND(RenderMainSceneViewport)(
			[ViewportRHI, View, Scene = MainScene.get(), RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
				CommandList.SwitchPipeline(ERHIPipeline::Graphics);
				CommandList.BeginDrawingViewport(ViewportRHI, nullptr);

				FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
				if (BackBuffer == nullptr)
				{
					CommandList.EndDrawingViewport(ViewportRHI, false, false);
					return;
				}

				if (RendererModule != nullptr)
				{
					RendererModule->PrepareSceneResources(CommandList, Scene);
				}
				if (RendererModule != nullptr)
				{
					FSceneView BackBufferView = View;
					BackBufferView.ViewportWidth = BackBuffer->GetSizeX();
					BackBufferView.ViewportHeight = BackBuffer->GetSizeY();
					RendererModule->RenderView(CommandList, Scene, BackBufferView, BackBuffer, true);
				}

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

	auto DEngine::GetActiveCameraComponent() const -> DCameraComponent*
	{
		return DefaultCameraActor != nullptr ? DefaultCameraActor->GetCameraComponent() : nullptr;
	}

	auto DEngine::BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView
	{
		FSceneView View;
		View.ViewportWidth = Width;
		View.ViewportHeight = Height;

		if (const DCameraComponent* CameraComponent = GetActiveCameraComponent())
		{
			const float AspectRatio = Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
			View.ViewMatrix = CameraComponent->GetViewMatrix();
			View.ProjectionMatrix = CameraComponent->GetProjectionMatrix(AspectRatio);
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewLocation = CameraComponent->GetWorldLocation();
		}

		return View;
	}

	DEngine* GEngine = nullptr;
} // namespace Durin
