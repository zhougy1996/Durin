#include "Engine/Engine.h"

#include "Asset/CookedMeshLoadManager.h"
#include "PrimitiveDrawInterface.h"
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
#include "Asset/Asset.h"
#include "Window/GenericWindow.h"

#include "DynamicRHI.h"
#include "SceneInterface.h"
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
	} // namespace

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
		if (!FModuleManager::Get().LoadModule("Engine"))
			return FEngineInitializationResult::Failure("Engine subsystem providers could not start.");
		if (!InitializeCookedMeshLoadManager())
			return FEngineInitializationResult::Failure(
				"Cooked mesh load manager could not start.");
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RegistryScanBegin);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.RegistryScan");
			const FAssetCatalogRefreshResult Refresh =
				RefreshAssetRegistry(
					EAssetRegistryScanMode::Incremental
				);
			if (!Refresh)
			{
				DURIN_ERROR(
					"Asset catalog refresh retained revision {} with {} error(s).",
					Refresh.ResultingRevision, Refresh.Errors.size());
				for (const FAssetRegistryResult& Error : Refresh.Errors)
				{
					DURIN_ERROR("Asset catalog refresh error: {}", Error.Message);
				}
			}
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RegistryScanComplete);
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
			if (MainSceneViewport)
				MainSceneViewport->InitializeViewState(RendererModule);
			for (const std::shared_ptr<FSceneViewport>& Viewport : AuxiliarySceneViewports)
				if (Viewport)
					Viewport->InitializeViewState(RendererModule);
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RendererReady);
		auto* World = NewObject<DWorld>(this, "MainWorld");
		World->SetWorldType(GetInitialWorldType());
		World->SetRenderScene(MainScene.get());
		if (auto Result = World->InitializeSubsystems(); !Result)
		{
			World->Shutdown();
			MarkObjectHierarchyAsGarbage(World);
			return FEngineInitializationResult::Failure(Result.Message);
		}
		SetWorld(World);
		Mona::FMonaApplication::Get().SetGameEventHandler(std::make_unique<FEngineInputEventHandler>());
		return FEngineInitializationResult::Success();
	}

	auto DEngine::BeginDestroy() -> void
	{
		ClearGameInputWindow();
		if (MainWorld) MainWorld->Shutdown();
		AuxiliarySceneViewports.clear();
		MainSceneViewport.reset();
		SetWorld(nullptr);
		if (MainScene != nullptr)
		{
			MainScene->Release();
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
		PumpCookedMeshLoadManager();
		if (GameInputWindow.expired() && GameInputState.IsEnabled()) ClearGameInputWindow();
		if (MainWorld) MainWorld->Tick({.DeltaSeconds = DeltaSeconds, .GameInput = &GameInputState});
		GameInputState.FinishGameTick();
	}

	auto DEngine::PrepareForShutdown() -> void
	{
		if (MainWorld) MainWorld->Shutdown();
		if (MainSceneViewport)
			MainSceneViewport->ReleaseViewState();
		for (const std::shared_ptr<FSceneViewport>& Viewport : AuxiliarySceneViewports)
			if (Viewport)
				Viewport->ReleaseViewState();
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
			const bool bCaptureRenderGraph =
				SceneViewport->ConsumeRenderGraphCaptureRequest();
			FSceneInterface* Scene = SceneViewport->GetRenderScene() != nullptr ? SceneViewport->GetRenderScene() : MainScene.get();
			ENQUEUE_RENDER_COMMAND(RenderSceneRenderTarget)(
				[RenderTargetRHI, View, Scene, SceneViewport,
				 RendererModule = RendererModule,
				 bCaptureRenderGraph](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					FSceneViewStatistics Statistics;
					auto RenderGraphCapture = bCaptureRenderGraph
						? std::make_shared<FRDGCapture>() : nullptr;
					ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
					if (RendererModule != nullptr)
					{
						const FSceneViewRenderOptions Options{};
						Result = RendererModule->RenderView(
							CommandList, Scene, View, RenderTargetRHI, false, Options,
							&Statistics, RenderGraphCapture.get()
						);
					}
					SceneViewport->PublishRenderStatistics_RenderThread(
						Statistics, Result == ERenderViewResult::Success
					);
					if (bCaptureRenderGraph)
						SceneViewport->PublishRenderGraphCapture_RenderThread(
							std::move(RenderGraphCapture),
							Result == ERenderViewResult::Success);
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
					const bool bCaptureRenderGraph =
						SceneViewport->ConsumeRenderGraphCaptureRequest();
					ENQUEUE_RENDER_COMMAND(RenderMainSceneViewport)(
						[ViewportRHI, View, SceneViewport, Scene = MainScene.get(),
						 RendererModule = RendererModule,
						 bCaptureRenderGraph](FRHICommandListImmediate& CommandList) {
							CommandList.SwitchPipeline(ERHIPipeline::Graphics);
							CommandList.BeginDrawingViewport(ViewportRHI, nullptr);

							FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
							if (BackBuffer == nullptr)
							{
								SceneViewport->PublishRenderStatistics_RenderThread({}, false);
								if (bCaptureRenderGraph)
									SceneViewport->PublishRenderGraphCapture_RenderThread(
										nullptr, false);
								CommandList.EndDrawingViewport(ViewportRHI, false, false);
								return;
							}

							FSceneViewStatistics Statistics;
							auto RenderGraphCapture = bCaptureRenderGraph
								? std::make_shared<FRDGCapture>() : nullptr;
							ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
							if (RendererModule != nullptr)
							{
								const FSceneViewRenderOptions Options{};
								Result = RendererModule->RenderView(
									CommandList, Scene, View, BackBuffer, true, Options,
									&Statistics, RenderGraphCapture.get()
								);
							}
							SceneViewport->PublishRenderStatistics_RenderThread(
								Statistics, Result == ERenderViewResult::Success
							);
							if (bCaptureRenderGraph)
								SceneViewport->PublishRenderGraphCapture_RenderThread(
									std::move(RenderGraphCapture),
									Result == ERenderViewResult::Success);

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
			MainSceneViewport->InitializeViewState(RendererModule);
			MainSceneViewport->RequestHistoryReset();
			MainSceneViewport->UpdateRHIViewport();
		}
	}

	auto DEngine::RegisterAuxiliarySceneViewport(const std::shared_ptr<FSceneViewport>& InSceneViewport) -> void
	{
		if (InSceneViewport == nullptr || InSceneViewport->IsWindowBacked()) return;
		InSceneViewport->InitializeViewState(RendererModule);
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
		LastCameraViewSource = nullptr;
		bHasCameraViewSource = false;
		if (MainSceneViewport)
			MainSceneViewport->RequestHistoryReset();
		for (const std::shared_ptr<FSceneViewport>& Viewport : AuxiliarySceneViewports)
			if (Viewport)
				Viewport->RequestHistoryReset();
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
		if (SceneViewport != nullptr)
			OutView.ViewStateId = SceneViewport->GetViewStateId();
		OutView.ViewportWidth = Width;
		OutView.ViewportHeight = Height;
		auto FinalizePersistentState = [&] {
			FViewPrimitiveDrawInterface(OutView).Seal();
			if (SceneViewport != nullptr)
			{
				OutView.ViewStateId = SceneViewport->GetViewStateId();
				OutView.bDiscardHistory =
					SceneViewport->ConsumeHistoryReset();
			}
		};
		if (SceneViewport != nullptr)
		{
			if (const FViewportClient* ViewportClient = SceneViewport->GetViewportClient())
			{
				const FSceneViewSettings ViewSettings = ViewportClient->GetViewSettings();
				if (ViewportClient->CalcSceneView(Width, Height, OutView))
				{
					OutView.Settings = ViewSettings;
					FinalizePersistentState();
					return true;
				}
				OutView.Settings = ViewSettings;
			}
		}

		if (bAllowCameraFallback)
		{
			if (const DCameraComponent* CameraComponent = GetActiveCameraComponent())
			{
				if (!bHasCameraViewSource
					|| LastCameraViewSource != CameraComponent)
				{
					if (SceneViewport != nullptr)
						SceneViewport->RequestHistoryReset();
					LastCameraViewSource = CameraComponent;
					bHasCameraViewSource = true;
				}
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
				SceneViewProjection::ClampViewDistances(OutView.FarClipDistance, ViewDistance.FadeStart, ViewDistance.RenderDistance, OutView.ViewFadeStart, OutView.ViewRenderDistance);
				FinalizePersistentState();
				return true;
			}
		}
		// The primary viewport historically renders a default identity view when no
		// camera exists; auxiliary viewports instead stay dormant without a client view.
		if (bAllowCameraFallback && bHasCameraViewSource
			&& LastCameraViewSource != nullptr)
		{
			if (SceneViewport != nullptr)
				SceneViewport->RequestHistoryReset();
			LastCameraViewSource = nullptr;
		}
		if (bAllowCameraFallback)
			FinalizePersistentState();
		return bAllowCameraFallback;
	}

	DEngine* GEngine = nullptr;
} // namespace Durin
