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
	namespace
	{
		auto ApplyCameraAspectRatio(const DCameraComponent& Camera, uint32 TargetWidth, uint32 TargetHeight, FSceneView& View) -> float
		{
			const float TargetAspectRatio = TargetHeight > 0 ? static_cast<float>(TargetWidth) / TargetHeight : 1.0f;
			const float CameraAspectRatio = Camera.ResolveAspectRatio(TargetAspectRatio);
			if (Camera.GetAspectRatioMode() == ECameraAspectRatioMode::Viewport) return CameraAspectRatio;
			View.AspectRatioConstraint = CameraAspectRatio;

			// Fit a centered content rectangle so a constrained camera never stretches on a differently shaped target.
			uint32 ContentWidth = TargetWidth;
			uint32 ContentHeight = static_cast<uint32>(std::round(ContentWidth / CameraAspectRatio));
			if (ContentHeight > TargetHeight)
			{
				ContentHeight = TargetHeight;
				ContentWidth = static_cast<uint32>(std::round(ContentHeight * CameraAspectRatio));
			}
			View.ViewportWidth = std::max(1u, ContentWidth);
			View.ViewportHeight = std::max(1u, ContentHeight);
			View.ViewportX = (TargetWidth - View.ViewportWidth) / 2;
			View.ViewportY = (TargetHeight - View.ViewportHeight) / 2;
			return CameraAspectRatio;
		}
	}

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
		SetWorld(NewObject<DWorld>(this, "MainWorld"));
		Mona::FMonaApplication::Get().SetGameEventHandler(std::make_unique<FEngineInputEventHandler>());
	}

	auto DEngine::BeginDestroy() -> void
	{
		AuxiliarySceneViewports.clear();
		MainSceneViewport.reset();
		SetWorld(nullptr);
		if (MainScene != nullptr)
		{
			MainScene->Release();
		}
		if (GRenderingThread)
		{
			DestroyFence = std::make_unique<FRenderCommandFence>();
			DestroyFence->BeginFence();
		}
		Super::BeginDestroy();
	}

	auto DEngine::IsReadyForFinishDestroy() -> bool
	{
		return Super::IsReadyForFinishDestroy()
			&& (!DestroyFence || DestroyFence->IsFenceComplete());
	}

	auto DEngine::FinishDestroy() -> void
	{
		check(!DestroyFence || DestroyFence->IsFenceComplete());
		MainScene.reset();
		DestroyFence.reset();
		RendererModule = nullptr;
		Super::FinishDestroy();
	}

	auto DEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		(void)bIdleMode;
		if (MainWorld) MainWorld->Tick(DeltaSeconds);
		GameInputState.FinishGameTick();
	}

	auto DEngine::RedrawViewports() -> void
	{
		auto RenderTargetViewport = [this](const std::shared_ptr<FSceneViewport>& SceneViewport, bool bAllowCameraFallback) {
			if (SceneViewport == nullptr || SceneViewport->IsWindowBacked()) return;
			const FVector2f DesiredSize = SceneViewport->GetDesiredSize();
			const uint32 ViewWidth = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.x)));
			const uint32 ViewHeight = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.y)));
			FSceneView View;
			if (!BuildSceneView(SceneViewport.get(), ViewWidth, ViewHeight, bAllowCameraFallback, View)) return;

			SceneViewport->UpdateRHIViewport();
			FTextureRHIRef RenderTargetRHI = SceneViewport->GetRenderTargetRHI();
			if (RenderTargetRHI == nullptr) return;
			IScene* Scene = SceneViewport->GetRenderScene() != nullptr ? SceneViewport->GetRenderScene() : MainScene.get();
			ENQUEUE_RENDER_COMMAND(RenderSceneRenderTarget)(
				[RenderTargetRHI, View, Scene, RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					if (RendererModule != nullptr)
					{
						RendererModule->RenderView(CommandList, Scene, View, RenderTargetRHI, false);
					}
				}
			);
		};

		if (MainSceneViewport != nullptr && !MainSceneViewport->IsWindowBacked())
		{
			RenderTargetViewport(MainSceneViewport, true);
		}
		else if (MainSceneViewport != nullptr)
		{
			MainSceneViewport->UpdateRHIViewport();
			const TRefCountPtr<FRHIViewport>& ViewportRHI = MainSceneViewport->GetRHIViewport();
			if (ViewportRHI != nullptr)
			{
				const FVector2f DesiredSize = MainSceneViewport->GetDesiredSize();
				const uint32 ViewWidth = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.x)));
				const uint32 ViewHeight = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.y)));
				FSceneView View;
				if (BuildSceneView(MainSceneViewport.get(), ViewWidth, ViewHeight, true, View))
				{
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
								RendererModule->RenderView(CommandList, Scene, View, BackBuffer, true);
							}

							CommandList.EndDrawingViewport(ViewportRHI, true, false);
						}
					);
				}
			}
		}

		for (const std::shared_ptr<FSceneViewport>& SceneViewport : AuxiliarySceneViewports)
		{
			RenderTargetViewport(SceneViewport, false);
		}
	}

	auto DEngine::SetMainSceneViewport(std::shared_ptr<FSceneViewport> InSceneViewport) -> void
	{
		MainSceneViewport = std::move(InSceneViewport);
		if (MainSceneViewport)
		{
			MainSceneViewport->UpdateRHIViewport();
		}
	}

	auto DEngine::RegisterAuxiliarySceneViewport(const std::shared_ptr<FSceneViewport>& InSceneViewport) -> void
	{
		if (InSceneViewport == nullptr || InSceneViewport->IsWindowBacked()) return;
		if (std::ranges::find(AuxiliarySceneViewports, InSceneViewport) == AuxiliarySceneViewports.end())
		{
			AuxiliarySceneViewports.push_back(InSceneViewport);
		}
	}

	auto DEngine::UnregisterAuxiliarySceneViewport(const FSceneViewport* InSceneViewport) -> void
	{
		std::erase_if(AuxiliarySceneViewports, [InSceneViewport](const std::shared_ptr<FSceneViewport>& Entry) {
			return Entry.get() == InSceneViewport;
		});
	}

	auto DEngine::SetWorld(DWorld* InWorld) -> void
	{
		if (MainWorld.Get() == InWorld) return;
		if (MainWorld) MainWorld->SetRenderScene(nullptr);
		MainWorld = InWorld;
		if (MainWorld) MainWorld->SetRenderScene(MainScene.get());
	}

	auto DEngine::SetGameInputEnabled(bool bEnabled) -> void
	{
		GameInputState.SetEnabled(bEnabled);
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
		BuildSceneView(MainSceneViewport.get(), Width, Height, true, View);
		return View;
	}

	auto DEngine::BuildSceneView(const FSceneViewport* SceneViewport, uint32 Width, uint32 Height, bool bAllowCameraFallback, FSceneView& OutView) const -> bool
	{
		OutView = {};
		OutView.ViewportWidth = Width;
		OutView.ViewportHeight = Height;
		if (SceneViewport != nullptr)
		{
			if (const FViewportClient* ViewportClient = SceneViewport->GetViewportClient())
			{
				if (ViewportClient->CalcSceneView(Width, Height, OutView))
				{
					OutView.Settings = ViewportClient->GetViewSettings();
					return true;
				}
			}
		}

		if (bAllowCameraFallback)
		{
			if (const DCameraComponent* CameraComponent = GetActiveCameraComponent())
			{
				const float AspectRatio = ApplyCameraAspectRatio(*CameraComponent, Width, Height, OutView);
				OutView.ViewMatrix = CameraComponent->GetViewMatrix();
				OutView.ProjectionMatrix = CameraComponent->GetProjectionMatrix(AspectRatio);
				OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
				OutView.ViewLocation = CameraComponent->GetWorldLocation();
				return true;
			}
		}
		// The primary viewport historically renders a default identity view when no
		// camera exists; auxiliary viewports instead stay dormant without a client view.
		return bAllowCameraFallback;
	}

	DEngine* GEngine = nullptr;
} // namespace Durin
