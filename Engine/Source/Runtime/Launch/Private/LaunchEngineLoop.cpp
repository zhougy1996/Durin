#include "LaunchEngineLoop.h"

#include "Threading/QueuedThreadPool.h"
#include "Threading/RunnableThread.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "ApplicationCore.h"
#include "AssetSystem.h"
#include "RHI.h"
#include "Mona.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
#include "RenderingThread.h"
#include "CoreGlobals.h"
#include "Misc/AppConfig.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/Time.h"

#include "Shader/ShaderPaths.h"
#include "DurinEngine.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	constexpr std::string_view AppConfigFileName = DURIN_PROFILE_NAME ".yaml";

	FEngineLoop GEngineLoop;

	auto FEngineLoop::PreInit(std::span<const std::string_view> Arguments) -> void
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		FPlatformMisc::AddRuntimeBinaryDirectory(FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());

		LoadAppConfig(FPaths::LaunchDir() + std::string(AppConfigFileName));

		FNameInit(); // Initialize FName system.
		LoggerInit();
		DURIN_DEBUG("Application name: {}", GAppConfig.GetView("AppName").GetString());
		DURIN_INFO(STR("Launching Durin engine..."));
		DURIN_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DURIN_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		std::string ProjectError;
		if (!InitializeCurrentProject(Arguments, &ProjectError) && !ProjectError.empty()) DURIN_WARN("{}", ProjectError);
		if (!FPaths::ProjectFile().empty()) DURIN_DEBUG(STR("Project file: {}"), FPaths::ProjectFile());
		PathUtilities::InitDefaultMountPoints(); // Initialize default mount points to enable path resolving.
		InitEngineThreadPool();

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
		const FYamlNodeView GarbageCollectionConfig = GAppConfig.GetView("GarbageCollection");
		FGarbageCollectionSettings GarbageCollectionSettings;
		GarbageCollectionSettings.bEnabled = GarbageCollectionConfig.GetView("Enabled").GetBool(true);
		GarbageCollectionSettings.IntervalSeconds = GarbageCollectionConfig.GetView("IntervalSeconds").GetDouble(60.0);
		GarbageCollectionSettings.PendingKillThreshold = GarbageCollectionConfig.GetView("PendingKillThreshold").GetUInt(128);
		GarbageCollectionSettings.ObjectGrowthThreshold = GarbageCollectionConfig.GetView("ObjectGrowthThreshold").GetUInt(1024);
		ConfigureAutomaticGarbageCollection(GarbageCollectionSettings, FTime::Seconds());
	}

	auto FEngineLoop::Init() -> void
	{
#if DURIN_WITH_EDITOR
		GEngine = NewObject<DEditorEngine>(nullptr, "EditorEngine");
#else
		GEngine = NewObject<DGameEngine>(nullptr, "GameEngine");
#endif
		AddToRoot(GEngine);

		InitializeApplicationCore();
		RHIInit();
		Mona::MonaInit();

		GEngine->Init();

		InitRenderingThread();
		DURIN_INFO(STR("Durin engine initialized."));
	}

	// Called from render thread
	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		check(IsInRenderingThread());
		GFrameCounterRenderThread = LogicFrameCounter;
		GRenderFrameCounterRenderThread = RenderFrameCounter;
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		GDynamicRHI->RHIBeginFrame();
	}

	// Called from render thread
	static auto EndFrameRenderThread(FRHICommandListImmediate& RHICmdList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		check(IsInRenderingThread());
		check(GFrameCounterRenderThread == LogicFrameCounter);
		check(GRenderFrameCounterRenderThread == RenderFrameCounter);
		GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
	}

	static auto RenderFrame() -> void
	{
		if (GDynamicRHI == nullptr)
		{
			return;
		}

		const uint64 LogicFrameCounter = GFrameCounter;
		const uint64 RenderFrameCounter = GRenderFrameCounter;

		ENQUEUE_RENDER_COMMAND(BeginFrame)([LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& CommandList) {
			BeginFrameRenderThread(CommandList, LogicFrameCounter, RenderFrameCounter);
		});

		if (GEngine != nullptr)
		{
			GEngine->RedrawViewports();
		}

		Mona::Render();

		ENQUEUE_RENDER_COMMAND(EndFrame)([LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& RHICmdList) {
			EndFrameRenderThread(RHICmdList, LogicFrameCounter, RenderFrameCounter);
		});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		GRenderFrameCounter++;
	}

	auto FEngineLoop::Tick() -> void
	{
		// Game logic.
		GEngine->Tick(0.0f, false);
		GFrameCounter++;

		// Process application events, and paint UI.
		Mona::FMonaApplication::Get().Tick();

		if (GIsRequestingExit) return;

		Mona::NewFrame();
		RenderFrame();
		const uint64 ObjectsBeforeGC = GDObjectArray.GetNum();
		const uint64 PendingKillBeforeGC = GetGarbageObjectCount();
		const double GCStartTime = FTime::Seconds();
		const EGarbageCollectionTrigger GCTrigger = TryCollectGarbage(GCStartTime);
		if (GCTrigger != EGarbageCollectionTrigger::None)
		{
			const char* TriggerName = GCTrigger == EGarbageCollectionTrigger::Interval ? "interval"
				: GCTrigger == EGarbageCollectionTrigger::PendingKillPressure ? "pending-kill pressure"
				: "object-growth pressure";
			DURIN_INFO("Automatic GC ({}) completed in {:.3f} ms: objects {} -> {}, pending kill {}.", TriggerName,
				(FTime::Seconds() - GCStartTime) * 1000.0, ObjectsBeforeGC, GDObjectArray.GetNum(), PendingKillBeforeGC);
		}

		CalculateFPSTimings();
	}

	auto FEngineLoop::Exit() -> void
	{
		Mona::MonaShutdown();

		ShutdownEngineThreadPool(true);

		RemoveFromRoot(GEngine);
		DestroyObject(GEngine);
		GEngine = nullptr;
		Asset::ShutdownAssetManager();
		CollectGarbage();

		FlushRenderingCommands();
		ShutdownRenderingThread();

		FModuleManager::Get().UnloadModulesAtShutdown();
		RHIExit();

		ShutdownApplicationCore();
		DURIN_INFO(STR("Durin Engine exited."));
	}
} // namespace Durin
