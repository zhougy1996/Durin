#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "CoreGlobals.h"

#include "Actors/CameraActor.h"
#include "Actors/PlayerController.h"
#include "Components/CameraComponent.h"
#include "SceneViewProjection.h"
#include "Client/ViewportClient.h"
#include "DObject/DObjectGlobals.h"
#include "IRendererModule.h"
#include "Client/SceneViewport.h"
#include "Modules/ModuleManager.h"
#include "Materials/DefaultMaterialService.h"
#include "Profiling/Profiling.h"
#include "Application/MonaApplication.h"
#include "Application/MonaEventHandler.h"
#include "Window/GenericWindow.h"

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
		auto IsTarget(const std::shared_ptr<FGenericWindow>& Window) const -> bool
		{
			return GEngine && GEngine->GameInputWindow.lock() == Window;
		}
		auto OnWindowFocused(const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void override
		{
			if (!IsTarget(Window)) return;
			GEngine->HandleGameInputWindowFocus(Window, bFocused);
			GEngine->GameInputState.SetFocused(bFocused);
		}
		auto OnWindowCloseRequested(const std::shared_ptr<FGenericWindow>& Window) -> bool override
		{
			if (!IsTarget(Window)) return false;
			GEngine->HandleGameInputWindowClose(Window);
			return false;
		}
		auto OnKeyDown(const std::shared_ptr<FGenericWindow>& Window, EKey Key, EKeyModFlags, bool bRepeat) -> bool override
		{
			if (!IsTarget(Window)) return false;
			if (GEngine->HandleGameInputKeyDown(Window, Key, bRepeat)) return true;
			GEngine->GameInputState.SetKey(Key, true);
			return GEngine->GameInputState.IsEnabled();
		}
		auto OnKeyUp(const std::shared_ptr<FGenericWindow>& Window, EKey Key, EKeyModFlags) -> bool override
		{
			if (!IsTarget(Window)) return false;
			GEngine->GameInputState.SetKey(Key, false);
			return GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseMove(const std::shared_ptr<FGenericWindow>& Window, FVector2d Position) -> bool override
		{
			if (!IsTarget(Window)) return false;
			GEngine->GameInputState.SetMousePosition(Position);
			return GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseDown(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button, FVector2d Position) -> bool override
		{
			if (!IsTarget(Window)) return false;
			if (GEngine->HandleGameInputMouseDown(Window, Button)) return true;
			GEngine->GameInputState.SetMousePosition(Position);
			GEngine->GameInputState.SetMouseButton(Button, true);
			return GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseUp(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button, FVector2d Position) -> bool override
		{
			if (!IsTarget(Window)) return false;
			GEngine->GameInputState.SetMousePosition(Position);
			GEngine->GameInputState.SetMouseButton(Button, false);
			return GEngine->GameInputState.IsEnabled();
		}
		auto OnMouseWheel(const std::shared_ptr<FGenericWindow>& Window, double, double DeltaY) -> bool override
		{
			if (!IsTarget(Window)) return false;
			GEngine->GameInputState.AddMouseWheel(DeltaY);
			return GEngine->GameInputState.IsEnabled();
		}
	};

	DEngine::DEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	DEngine::~DEngine() = default;

	auto DEngine::Init(const FEngineInitContext&) -> FEngineInitializationResult
	{
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultMaterialBegin);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.DefaultMaterial");
			InitializeDefaultMaterialService();
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultMaterialReady);
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RendererBegin);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.RendererInitialization");
			RendererModule = &FModuleManager::LoadModuleChecked<IRendererModule>("Renderer");
			MainScene = RendererModule->CreateScene();
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RendererReady);
		SetWorld(NewObject<DWorld>(this, "MainWorld"));
		Mona::FMonaApplication::Get().SetGameEventHandler(std::make_unique<FEngineInputEventHandler>());
		return FEngineInitializationResult::Success();
	}

	auto DEngine::BeginDestroy() -> void
	{
		ClearGameInputWindow();
		AuxiliarySceneViewports.clear();
		MainSceneViewport.reset();
		SetWorld(nullptr);
		if (MainScene != nullptr)
		{
			MainScene.reset();
		}
		ShutdownDefaultMaterialService();
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
		DestroyFence.reset();
		RendererModule = nullptr;
		Super::FinishDestroy();
	}

	auto DEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		(void)bIdleMode;
		if (GameInputWindow.expired() && GameInputState.IsEnabled()) ClearGameInputWindow();
		if (MainWorld) MainWorld->Tick({.DeltaSeconds = DeltaSeconds, .GameInput = &GameInputState});
		GameInputState.FinishGameTick();
	}

	auto DEngine::PrepareForShutdown() -> void
	{
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
				[RenderTargetRHI, View, Scene, SceneViewport,
					RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					FSceneViewStatistics Statistics;
					ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
					if (RendererModule != nullptr)
					{
						Result = RendererModule->RenderView(
							CommandList, Scene, View, RenderTargetRHI, false, {},
							&Statistics);
					}
					SceneViewport->PublishRenderStatistics_RenderThread(
						Statistics, Result == ERenderViewResult::Success);
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
					const std::shared_ptr<FSceneViewport> SceneViewport = MainSceneViewport;
					ENQUEUE_RENDER_COMMAND(RenderMainSceneViewport)(
						[ViewportRHI, View, SceneViewport, Scene = MainScene.get(),
							RendererModule = RendererModule](FRHICommandListImmediate& CommandList) {
							CommandList.SwitchPipeline(ERHIPipeline::Graphics);
							CommandList.BeginDrawingViewport(ViewportRHI, nullptr);

							FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
							if (BackBuffer == nullptr)
							{
								SceneViewport->PublishRenderStatistics_RenderThread({}, false);
								CommandList.EndDrawingViewport(ViewportRHI, false, false);
								return;
							}

							FSceneViewStatistics Statistics;
							ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
							if (RendererModule != nullptr)
							{
								Result = RendererModule->RenderView(
									CommandList, Scene, View, BackBuffer, true, {},
									&Statistics);
							}
							SceneViewport->PublishRenderStatistics_RenderThread(
								Statistics, Result == ERenderViewResult::Success);

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
		GameInputState.SetEnabled(bEnabled && !GameInputWindow.expired());
	}

	auto DEngine::SetGameInputWindow(const std::shared_ptr<FGenericWindow>& InWindow) -> void
	{
		if (GameInputWindow.lock() == InWindow && (InWindow || !GameInputState.IsEnabled())) return;
		GameInputState.SetEnabled(false);
		GameInputState.SetFocused(false);
		GameInputWindow = InWindow;
		if (InWindow) GameInputState.SetFocused(InWindow->IsFocused());
	}

	auto DEngine::ClearGameInputWindow() -> void
	{
		SetGameInputWindow(nullptr);
	}

	auto DEngine::ResetGameInputMouse() -> void
	{
		GameInputState.ResetMouseTracking();
	}

	auto DEngine::GetActiveCameraComponent() const -> DCameraComponent*
	{
		if (MainWorld && MainWorld->GetCurrentLevel())
		{
			if (APlayerController* Controller = MainWorld->GetLocalPlayerController(); Controller
				&& MainWorld->ContainsActor(Controller))
			{
				AActor* Target = Controller->GetViewTarget();
				if (Target
					&& !Target->IsPendingKill()
					&& !Target->IsBeingDestroyed()
					&& MainWorld->ContainsActor(Target))
				{
					if (DCameraComponent* Camera = Target->FindComponentByClass<DCameraComponent>()) return Camera;
				}
			}
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
				OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
				OutView.NearClipDistance = CameraComponent->GetNearClip();
				OutView.FarClipDistance = CameraComponent->GetFarClip();
				const FViewDistanceSettings& ViewDistance =
					CameraComponent->GetViewDistance();
				SceneViewProjection::ClampViewDistances(OutView.FarClipDistance,
					ViewDistance.FadeStart, ViewDistance.RenderDistance,
					OutView.ViewFadeStart, OutView.ViewRenderDistance);
				return true;
			}
		}
		// The primary viewport historically renders a default identity view when no
		// camera exists; auxiliary viewports instead stay dormant without a client view.
		return bAllowCameraFallback;
	}

	DEngine* GEngine = nullptr;
} // namespace Durin
