#include "Engine/Engine.h"
#include "Engine/MapChangeHandler.h"
#include "Rendering/ViewportPresentation.h"

#include "Asset/CookedMeshLoadManager.h"
#include "Rendering/PrimitiveDrawInterface.h"
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
#include "Components/PrimitiveComponent.h"
#include "Profiling/Profiling.h"
#include "Application/MonaApplication.h"
#include "Application/MonaEventHandler.h"
#include "Asset/Asset.h"
#include "Window/GenericWindow.h"

#include "DynamicRHI.h"
#include "Rendering/SceneInterface.h"
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
			if (!bFocused && GEngine->MainWorld)
				if (auto* Controller = GEngine->MainWorld->GetLocalPlayerController()) Controller->CancelPlayerInput();
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
			GEngine->GameInputState.SetKey(Key, true, bRepeat);
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
		: Super(ObjectInitializer), EngineSubsystems(*this)
	{
	}

	DEngine::~DEngine() = default;

	DEngine::FHostOperationScope::FHostOperationScope(DEngine& InHost, bool bInFlushOnExit) : Host(InHost), bFlushOnExit(bInFlushOnExit) { ++Host.HostOperationDepth; }
	auto DEngine::BeginTransaction(std::string_view, std::string_view, DObject*) -> int32 { return -1; }
	auto DEngine::EndTransaction() -> int32 { return 0; }
	auto DEngine::CancelTransaction(int32) -> void {}
	DEngine::FHostOperationScope::~FHostOperationScope()
	{
		if (--Host.HostOperationDepth == 0 && bFlushOnExit && Host.bShutdownRequested) Host.PrepareForShutdown();
	}

	DEngine::FWorldInitializationScope::FWorldInitializationScope(DEngine& InHost, DWorld& InWorld)
		: Operation(InHost), Host(InHost), World(InWorld)
	{
		Host.InitializingWorlds.push_back(&World);
		if (Host.bShutdownRequested) World.RequestShutdown();
	}

	DEngine::FWorldInitializationScope::~FWorldInitializationScope()
	{
		std::erase(Host.InitializingWorlds, &World);
	}

	auto DEngine::DispatchSubsystemCallback(std::function<void()> Callback) -> void
	{
		if (bShutdownRequested) return;
		FHostOperationScope Operation(*this, false);
		Callback();
	}

	auto DEngine::InitializeEngineSubsystems() -> FSubsystemResult
	{
		FHostOperationScope Operation(*this);
		return EngineSubsystems.Initialize();
	}

	auto DEngine::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		EngineSubsystems.AddReferencedObjects(Collector);
		for (DWorld* World : InitializingWorlds)
		{
			DObject* Object = World;
			Collector.AddReferencedObject(Object);
		}
	}

	auto DEngine::Init(const FEngineInitContext& Context) -> bool
	{
		if (bInitStarted || bShutdownRequested)
		{
			DURIN_ERROR("Engine initialization is one-shot.");
			return false;
		}
		bInitStarted = true;
		FHostOperationScope Operation(*this);
		bool bInitialized = false;
		try { bInitialized = InitInternal(Context); }
		catch (...) { DURIN_ERROR("Engine initialization threw an exception."); }
		if (bShutdownRequested && bInitialized)
		{
			DURIN_ERROR("Engine retired during initialization.");
			bInitialized = false;
		}
		if (!bInitialized) PrepareForShutdown();
		return bInitialized;
	}

	auto DEngine::InitInternal(const FEngineInitContext&) -> bool
	{
		if (!FModuleManager::Get().LoadModule("Engine"))
		{
			DURIN_ERROR("Engine subsystem providers could not start.");
			return false;
		}
		if (!InitializeCookedMeshLoadManager())
		{
			DURIN_ERROR("Cooked mesh load manager could not start.");
			return false;
		}
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
		Mona::FMonaApplication::Get().SetGameEventHandler(std::make_unique<FEngineInputEventHandler>());
		return true;
	}

	auto DEngine::InitializeProjectContent() -> std::expected<void, std::string>
	{
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RegistryScanBegin);
		const auto Refresh = RefreshAssetRegistry(EAssetRegistryScanMode::Incremental);
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RegistryScanComplete);
		if (!Refresh)
		{
			std::string Error = "Initial project asset catalog could not be admitted.";
			for (const auto& Cause : Refresh.Errors) Error += "\n" + FormatAssetRegistryError(Cause);
			return std::unexpected(std::move(Error));
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultMaterialBegin);
		// The service owns its explicit ErrorMaterial fallback policy.
		(void)InitializeDefaultMaterialService();
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultMaterialReady);
		// Project modules may add providers after the minimal shell's services.
		// The first World must capture the complete admitted provider set.
		if (const auto Services = InitializeEngineSubsystems(); !Services)
			return std::unexpected(Services.Message);
		auto* World = NewObject<DWorld>(this, "MainWorld");
		World->SetWorldType(GetInitialWorldType());
		World->SetRenderScene(MainScene.get());
		if (const auto Services = World->InitializeSubsystems(); !Services)
		{
			World->Shutdown();
			MarkObjectHierarchyAsGarbage(World);
			return std::unexpected(Services.Message);
		}
		SetWorld(World);
		return {};
	}

	auto DEngine::BeginDestroy() -> void
	{
		PrepareForShutdown();
		ClearGameInputWindow();
		MapChange.reset();
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
		return HostOperationDepth == 0 && !bPreparingShutdown && Super::IsReadyForFinishDestroy()
			   && (!DestroyFence || DestroyFence->IsFenceComplete());
	}

	auto DEngine::FinishDestroy() -> void
	{
		check(!DestroyFence || DestroyFence->IsFenceComplete());
		DestroyFence.reset();
		RendererModule = nullptr;
		Super::FinishDestroy();
	}

	auto DEngine::GetStartupError() const -> const std::string&
	{
		static const std::string Empty;
		return Empty;
	}

	auto DEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		if (bShutdownRequested) { PrepareForShutdown(); return; }
		FHostOperationScope Operation(*this);
		(void)bIdleMode;
		PumpCookedMeshLoadManager();
		DPrimitiveComponent::ProcessPSOPrecacheRequests();
		if (GameInputWindow.expired() && GameInputState.IsEnabled()) ClearGameInputWindow();
		if (const auto Window = GameInputWindow.lock(); Window && Mona::FMonaApplication::IsInitialized())
		{
			const auto Capture = Mona::FMonaApplication::Get().GetInputCapture(Window);
			GameInputState.bKeyboardBlocked = Capture.bKeyboard;
			GameInputState.bMouseBlocked = Capture.bMouse;
			if (MainWorld)
				if (auto* Controller = MainWorld->GetLocalPlayerController())
					Controller->GetInputActions().SetDeviceBlocked(Capture.bKeyboard, Capture.bMouse);
		}
		if (MainWorld) MainWorld->Tick({.DeltaSeconds = DeltaSeconds, .GameInput = &GameInputState});
		GameInputState.FinishGameTick();
	}

	auto DEngine::AreHostConsumersIdle() -> bool
	{
		for (const auto& Context : WorldContexts)
			if (auto* World = Context.World(); World && !World->IsReadyForFinishDestroy()) return false;
		return !MainWorld || MainWorld->IsReadyForFinishDestroy();
	}

	auto DEngine::CloseSubsystemWork() -> void
	{
		CancelPendingMapChange();
		// Host retirement closes callback owners before another ordinary frame.
		if (MapChange) MapChange->OnComplete = {};
		EngineSubsystems.CloseWork();
		const auto Contexts = WorldContexts;
		for (const auto& Context : Contexts) if (auto* World = Context.World()) World->RequestShutdown();
		for (DWorld* World : InitializingWorlds) World->RequestShutdown();
	}

	auto DEngine::PrepareForShutdown() -> void
	{
		bShutdownRequested = true;
		CloseSubsystemWork();
		if (HostOperationDepth || bPreparingShutdown || bShutdownComplete || !AreHostConsumersIdle()) return;
		bPreparingShutdown = true;
		FGarbageCollectionDeferralScope Deferral;
		RetireHostConsumers();
		const auto Contexts = WorldContexts;
		for (const auto& Context : Contexts) if (auto* World = Context.World()) World->Shutdown();
		EngineSubsystems.Shutdown();
		if (MainSceneViewport) MainSceneViewport->ReleaseViewState();
		for (const auto& Viewport : AuxiliarySceneViewports) if (Viewport) Viewport->ReleaseViewState();
		bShutdownComplete = true;
		bPreparingShutdown = false;
	}

	auto DEngine::RedrawViewports() -> void
	{
		SubmitPendingMapChange();
		if (bShutdownRequested || (GetStartupState() != EHostStartupState::Ready && !ShouldDrawStartupScene())) return;
		auto RenderTargetViewport = [this](const std::shared_ptr<FSceneViewport>& SceneViewport, bool bAllowCameraFallback) {
			if (SceneViewport == nullptr || SceneViewport->IsWindowBacked()) return;
			if (!SceneViewport->ConsumeDisplayRequest()) return;
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
				[RenderTargetRHI, View, Scene, SceneViewport, Generation = SceneViewport->GetSceneGeneration(), Receipt = SceneViewport->GetOutputReceipt(),
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
						const FSceneViewRenderRequest Request{Scene, View, RenderTargetRHI, false, Options, &Statistics, RenderGraphCapture.get()};
						Result = SceneViewport->RenderView_RenderThread(*RendererModule, CommandList, Request);
					}
					SceneViewport->PublishRenderStatistics_RenderThread(
						Statistics, Result == ERenderViewResult::Success
					);
					SceneViewport->PublishRenderOutcome_RenderThread(Result);
					Receipt->RecordProduced(Result == ERenderViewResult::Success ? Generation : 0);
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
						 Generation = SceneViewport->GetSceneGeneration(), Receipt = SceneViewport->GetOutputReceipt(),
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
								const FSceneViewRenderRequest Request{Scene, View, BackBuffer, true, Options, &Statistics, RenderGraphCapture.get()};
								Result = SceneViewport->RenderView_RenderThread(*RendererModule, CommandList, Request);
							}
							SceneViewport->PublishRenderStatistics_RenderThread(
								Statistics, Result == ERenderViewResult::Success
							);
							if (bCaptureRenderGraph)
								SceneViewport->PublishRenderGraphCapture_RenderThread(
									std::move(RenderGraphCapture),
									Result == ERenderViewResult::Success);

							SceneViewport->PublishRenderOutcome_RenderThread(Result);
							Receipt->RecordProduced(Result == ERenderViewResult::Success ? Generation : 0);
							EndDrawingViewportWithReceipts(CommandList, ViewportRHI,
								Result == ERenderViewResult::Success, {{Receipt, Generation}});
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
		if (!std::ranges::contains(AuxiliarySceneViewports, InSceneViewport))
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

	auto DEngine::PrepareMapChange(FMapChangeRequest Request) -> bool
	{
		CheckGameThread();
		if (bShutdownRequested || MapChange) return false;
		LastMapChangeProgress.reset();
		MapChange.reset(new FMapChangeHandler(*this, std::move(Request)));
		return true;
	}
	auto DEngine::OpenLevel(FMapChangeRequest Request) -> bool
	{
		return PrepareMapChange(std::move(Request)) && CommitMapChange();
	}
	auto DEngine::OpenLevel(const FObjectPath& Path) -> bool
	{
		return OpenLevel(FMapChangeRequest{.Path = Path});
	}
	auto DEngine::CommitMapChange() -> bool
	{
		CheckGameThread();
		if (bShutdownRequested || !MapChange) return false;
		const auto Phase = MapChange->GetProgress().Phase;
		if (Phase == EMapChangePhase::Committed || Phase == EMapChangePhase::Canceled || Phase == EMapChangePhase::Failed) return false;
		MapChange->bCommitRequested = true;
		return true;
	}
	auto DEngine::CancelPendingMapChange() -> void
	{
		CheckGameThread();
		if (MapChange) MapChange->Cancel();
	}
	auto DEngine::SubmitPendingMapChange() -> void
	{
		if (bShutdownRequested || !MapChange) return;
		FHostOperationScope Operation(*this);
		MapChange->SubmitView();
	}
	auto DEngine::IsPreparingMapChange() const -> bool { return MapChange != nullptr; }
	auto DEngine::IsMapChangeReady() const -> bool
	{
		CheckGameThread();
		return MapChange && MapChange->GetProgress().Phase == EMapChangePhase::Ready;
	}
	auto DEngine::GetMapChangeProgress() const -> std::optional<FMapChangeProgress>
	{
		CheckGameThread();
		return MapChange ? MapChange->GetProgress() : LastMapChangeProgress;
	}
	// Only the ordinary frame advances and completes changes; UI callbacks request work.
	auto DEngine::TickMapChange() -> void
	{
		if (bShutdownRequested || !MapChange) return;
		FHostOperationScope Operation(*this);
		MapChange->Advance();
		if (IsMapChangeReady() && MapChange->bCommitRequested) MapChange->Commit();
		const auto Progress = MapChange->GetProgress();
		if (Progress.Phase != EMapChangePhase::Committed && Progress.Phase != EMapChangePhase::Canceled && Progress.Phase != EMapChangePhase::Failed) return;
		auto OnComplete = std::move(MapChange->OnComplete);
		LastMapChangeProgress = Progress;
		MapChange.reset();
		if (OnComplete) OnComplete(Progress);
	}
	auto DEngine::AdoptSceneWorld(DWorld* World, FScenePtr Scene, uint64 Generation) -> void
	{
		CheckGameThread();
		check(World && Scene && !bShutdownRequested);
		FHostOperationScope Operation(*this);
		TStrongObjectPtr<DWorld> Previous(MainWorld.Get());
		if (Previous) Previous->SetRenderScene(nullptr);
		if (MainScene) { MainScene->Release(); MainScene.reset(); }
		MainScene = std::move(Scene);
		SceneGeneration = Generation;
		if (MainSceneViewport) MainSceneViewport->SetSceneGeneration(Generation);
		for (const auto& Viewport : AuxiliarySceneViewports) if (Viewport) Viewport->SetSceneGeneration(Generation);
		SetWorld(World);
		if (Previous && Previous.Get() != World) Previous->Shutdown();
	}
	auto DEngine::SetWorld(DWorld* InWorld) -> void
	{
		if (InWorld) RegisterWorldContext(*InWorld);
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
	auto DEngine::RegisterWorldContext(DWorld& World) -> void
	{
		CheckGameThread();
		for (auto& Context : WorldContexts)
			if (Context.World() == &World) { Context.WorldType = World.GetWorldType(); return; }
		FWorldContext Context;
		Context.WorldType = World.GetWorldType(); Context.CurrentWorld = &World;
		WorldContexts.push_back(std::move(Context));
	}
	auto DEngine::DestroyWorldContext(DWorld* World) -> void
	{
		CheckGameThread();
		std::erase_if(WorldContexts, [&](const auto& Context) { return !Context.World() || Context.World() == World; });
	}
	auto DEngine::GetWorldContextFromWorld(const DWorld* World) const -> const FWorldContext*
	{
		if (!World) return nullptr;
		for (const auto& Context : WorldContexts) if (Context.World() == World) return &Context;
		return nullptr;
	}

	auto DEngine::SetGameInputEnabled(bool bEnabled) -> void
	{
		GameInputState.SetEnabled(bEnabled && !GameInputWindow.expired());
		if (!GameInputState.IsEnabled() && MainWorld)
			if (auto* Controller = MainWorld->GetLocalPlayerController()) Controller->CancelPlayerInput();
	}

	auto DEngine::SetGameInputWindow(const std::shared_ptr<FGenericWindow>& InWindow) -> void
	{
		if (GameInputWindow.lock() == InWindow && (InWindow || !GameInputState.IsEnabled())) return;
		SetGameInputEnabled(false);
		GameInputState.bKeyboardBlocked = GameInputState.bMouseBlocked = false;
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

	static auto GetWorldCamera(const DWorld* World) -> DCameraComponent*
	{
		if (World && World->GetCurrentLevel())
		{
			if (APlayerController* Controller = World->GetLocalPlayerController(); Controller
																					   && World->ContainsActor(Controller))
			{
				AActor* Target = Controller->GetViewTarget();
				if (Target
					&& !Target->IsPendingKill()
					&& !Target->IsBeingDestroyed()
					&& World->ContainsActor(Target))
				{
					if (DCameraComponent* Camera = Target->FindComponentByClass<DCameraComponent>()) return Camera;
				}
			}
			if (ACameraActor* Camera = World->GetCurrentLevel()->GetPrimaryCameraActor()) return Camera->GetCameraComponent();
		}
		return nullptr;
	}

	auto DEngine::GetActiveCameraComponent() const -> DCameraComponent* { return GetWorldCamera(MainWorld.Get()); }

	auto DEngine::BuildWorldSceneView(const DWorld& World, uint32 Width, uint32 Height) const -> FSceneView
	{
		FSceneView View;
		View.ViewportWidth = Width; View.ViewportHeight = Height;
		if (const auto* Camera = GetWorldCamera(&World))
		{
			const float AspectRatio = ApplyCameraAspectRatio(*Camera, Width, Height, View);
			View.ViewMatrix = Camera->GetViewMatrix();
			View.ProjectionMatrix = Camera->GetProjectionMatrix(AspectRatio);
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewLocation = Camera->GetWorldLocation();
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.NearClipDistance = Camera->GetNearClip();
			View.FarClipDistance = Camera->GetFarClip();
			const auto& Distance = Camera->GetViewDistance();
			SceneViewProjection::ClampViewDistances(View.FarClipDistance, Distance.FadeStart, Distance.RenderDistance, View.ViewFadeStart, View.ViewRenderDistance);
		}
		FViewPrimitiveDrawInterface(View).Seal();
		return View;
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
