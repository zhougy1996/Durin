#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "CoreGlobals.h"

#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Client/ViewportClient.h"
#include "DObject/DObjectGlobals.h"
#include "IRendererModule.h"
#include "Mona/SceneViewport.h"
#include "Modules/ModuleManager.h"
#include "Application/MonaApplication.h"
#include "Application/MonaEventHandler.h"

#include "DynamicRHI.h"
#include "IScene.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	class FEngineInputEventHandler final : public Mona::FMonaEventHandler
	{
	public:
		auto OnWindowFocused(const std::shared_ptr<FGenericWindow>&, bool bFocused) -> void override
		{
			if (GEngine) GEngine->GameInputState.SetFocused(bFocused);
		}
		auto OnKeyDown(const std::shared_ptr<FGenericWindow>&, EKey Key, EKeyModFlags, bool) -> bool override
		{
			if (GEngine) GEngine->GameInputState.SetKey(Key, true);
			return GEngine && GEngine->GameInputState.IsEnabled();
		}
		auto OnKeyUp(const std::shared_ptr<FGenericWindow>&, EKey Key, EKeyModFlags) -> bool override
		{
			if (GEngine) GEngine->GameInputState.SetKey(Key, false);
			return GEngine && GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseMove(const std::shared_ptr<FGenericWindow>&, FVector2d Position) -> bool override
		{
			if (GEngine) GEngine->GameInputState.SetMousePosition(Position);
			return GEngine && GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseDown(const std::shared_ptr<FGenericWindow>&, EMouseButton Button, FVector2d Position) -> bool override
		{
			if (GEngine)
			{
				GEngine->GameInputState.SetMousePosition(Position);
				GEngine->GameInputState.SetMouseButton(Button, true);
			}
			return GEngine && GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseUp(const std::shared_ptr<FGenericWindow>&, EMouseButton Button, FVector2d Position) -> bool override
		{
			if (GEngine)
			{
				GEngine->GameInputState.SetMousePosition(Position);
				GEngine->GameInputState.SetMouseButton(Button, false);
			}
			return GEngine && GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseWheel(const std::shared_ptr<FGenericWindow>&, double, double DeltaY) -> bool override
		{
			if (GEngine) GEngine->GameInputState.AddMouseWheel(DeltaY);
			return GEngine && GEngine->GameInputState.IsEnabled();
		}
	};

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
		Mona::FMonaApplication::Get().SetGameEventHandler(std::make_unique<FEngineInputEventHandler>());
	}

	auto DEngine::BeginDestroy() -> void
	{
		MainSceneViewport.reset();
		MainWorld = nullptr;
		if (MainScene != nullptr)
		{
			MainScene->Release();
		}
		if (RendererModule != nullptr) RendererModule->ReleaseResources();
		// Tests and tools can construct an engine object without starting the render
		// thread; a fence cannot complete in that state.
		if (GRenderingThread) FlushRenderingCommands();
		MainScene.reset();
		RendererModule = nullptr;
		Super::BeginDestroy();
	}

	auto DEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		(void)bIdleMode;
		if (MainWorld) MainWorld->Tick(DeltaSeconds);
		GameInputState.FinishGameTick();
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

	auto DEngine::SetWorld(DWorld* InWorld) -> void
	{
		MainWorld = InWorld;
	}

	auto DEngine::GetActiveCameraComponent() const -> DCameraComponent*
	{
		if (MainWorld && MainWorld->GetCurrentLevel())
		{
			if (ACameraActor* Camera = MainWorld->GetCurrentLevel()->GetPrimaryCameraActor()) return Camera->GetCameraComponent();
		}
		return nullptr;
	}

	auto DEngine::BuildMainSceneView(uint32 Width, uint32 Height) const -> FSceneView
	{
		FSceneView View;
		View.ViewportWidth = Width;
		View.ViewportHeight = Height;
		if (MainSceneViewport != nullptr)
		{
			if (const FViewportClient* ViewportClient = MainSceneViewport->GetViewportClient())
			{
				if (ViewportClient->CalcSceneView(Width, Height, View)) return View;
			}
		}

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
