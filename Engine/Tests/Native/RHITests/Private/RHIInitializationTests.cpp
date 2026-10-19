#include <gtest/gtest.h>

#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHIThread.h"
#include "RHIContext.h"
#include "RHICommandList.h"
#include "NativeRHIExecutionTestSupport.h"
#include "Threading/Runnable.h"
#include "Threading/ThreadEvent.h"
#include "Threading/RunnableThread.h"

namespace Durin
{

	namespace
	{
		class FRHILifecycleTestEnvironment final : public testing::Environment
		{
			auto SetUp() -> void override
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		};
		[[maybe_unused]] auto* LifecycleTestEnvironment =
			testing::AddGlobalTestEnvironment(new FRHILifecycleTestEnvironment);

		class FBlockedRenderer final : public FRunnable
		{
		public:
			FThreadEvent Ready, Release;
			auto Run() -> uint32 override
			{
				Ready.Trigger();
				Release.Wait();
				return 0;
			}
		};

		class FInitializationCommandContext final : public IRHICommandContext
		{
		public:
			auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override {}
			auto RHISubmitCommands() -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHIBeginDiagnosticRegion(std::string_view Name) -> void override {}
			auto RHIEndDiagnosticRegion() -> void override {}
			auto RHIBeginRenderPass(const FRHIRenderPassInfo& InInfo, FName InName) -> void override {}
			auto RHIEndRenderPass() -> void override {}
			auto RHIBeginDrawingViewport(FRHIViewport* InViewport, FRHITexture* InRenderTargetRHI) -> void override {}
			auto RHIEndDrawingViewport(FRHIViewport* InViewport, bool bInPresent, bool bInLockToVsync) -> void override {}
			auto RHISetViewport(float InMinX, float InMinY, float InMinZ, float InMaxX, float InMaxY, float InMaxZ) -> void override {}
			auto RHISetScissor(float InMinX, float InMinY, float InWidth, float InHeight) -> void override {}
			auto RHISetDepthBias(float InConstantFactor, float InClamp, float InSlopeFactor) -> void override {}
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& InGraphicsPipelineState) -> void override {}
			auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void override {}
			auto RHIBindIndexBuffer(FRHIBuffer* IndexBuffer, uint32 Offset) -> void override {}
			auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void override {}
			auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override {}
			auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination, std::span<const FRHIBufferCopyRegion> Regions) -> void override {}
			auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override {}
			auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override {}
			auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination, std::span<const FRHITextureCopyRegion> Regions) -> void override {}
			auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void override {}
			auto RHIInitializeTexture(FRHITexture* Texture) -> void override {}
			auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, FByteView SourceData) -> void override {}
			auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex, const FUpdateTextureRegion3D& UpdateRegion, uint32 SourceRowPitch, uint32 SourceDepthPitch, FByteView SourceData) -> void override {}
			auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, FByteBuffer& OutData) -> bool override { return false; }
			auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void override {}
			auto RHIBlockUntilGPUIdle() -> void override {}
			auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override {}
			auto RHISetShaderParameters(FRHIShader* InShader, const std::span<const FRHIShaderParameterResource>& InResourceParameters) -> void override {}
			auto RHIDraw(const FRHIDrawArguments& Arguments) -> void override {}
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void override {}
		};

		struct FInitializationObservation
		{
			uint32 InitCount = 0;
			uint32 InitCleanupCount = 0;
			uint32 ShutdownCount = 0;
			uint32 DestructionCount = 0;
			bool bInitOnRHIThread = false;
			bool bInitCleanupOnRHIThread = false;
			bool bShutdownOnRHIThread = false;
			bool bInitExecutionAllowed = false;
			bool bShutdownExecutionAllowed = false;
			bool bDestructionExecutionAllowed = false;
			bool bCapabilitiesClearedAtShutdown = false;
			bool bFailShutdown = false;
			bool bUnknownInitFailure = false;
			FRHIInitializationContext InitializationContext =
				FRHIInitializationContext::Headless();
		};

		class FFailingDynamicRHI final : public FDynamicRHI
		{
		public:
			explicit FFailingDynamicRHI(
				FInitializationObservation& InObservation,
				bool bInFailInit = true)
				: Observation(InObservation)
				, bFailInit(bInFailInit)
			{
			}

			~FFailingDynamicRHI() override
			{
				Observation.bDestructionExecutionAllowed = IsInRHIExecutionThread();
				++Observation.DestructionCount;
			}

			auto Init(const FRHIInitializationContext& Context) -> void override
			{
				Observation.bInitExecutionAllowed = IsInRHIExecutionThread();
				++Observation.InitCount;
				Observation.InitializationContext = Context;
				Observation.bInitOnRHIThread = IsInRHIThread();
				if (bFailInit)
				{
					++Observation.InitCleanupCount;
					Observation.bInitCleanupOnRHIThread = IsInRHIThread();
					ClearCapabilities();
					if (Observation.bUnknownInitFailure) throw 42;
					throw std::runtime_error("intentional backend init failure");
				}
				FRHICapabilities Capabilities;
				Capabilities.SupportedTextureDimensions =
					ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube;
				Capabilities.MaxTextureDimension2D = 4096;
				Capabilities.MaxTextureDimensionCube = 2048;
				Capabilities.MaxTextureArrayLayers = 256;
				Capabilities.ColorSampleCounts = ERHISampleCountFlags::Samples1 | ERHISampleCountFlags::Samples4;
				Capabilities.DepthSampleCounts = ERHISampleCountFlags::Samples1;
				Capabilities.MinStorageBufferOffsetAlignment = 16;
				Capabilities.MaxStorageBufferRange = 128 * 1024 * 1024;
				Capabilities.MaxComputeWorkGroupCount = {65535, 1024, 64};
				PublishCapabilities(Capabilities);
			}

			auto Shutdown() -> void override
			{
				Observation.bShutdownExecutionAllowed = IsInRHIExecutionThread();
				++Observation.ShutdownCount;
				Observation.bShutdownOnRHIThread = IsInRHIThread();
				ClearCapabilities();
				Observation.bCapabilitiesClearedAtShutdown = RHIGetCapabilities() == nullptr;
				if (Observation.bFailShutdown) throw std::runtime_error("intentional rollback failure");
			}

			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHICreateViewport(
				const FRHIViewportCreateInfo&)
				-> TRefCountPtr<FRHIViewport> override { return {}; }
			auto RHIResizeViewport(
				FRHIViewport*, uint32, uint32, bool) -> void override {}
			auto RHICreateGraphicsPipelineState(
				FName, const FGraphicsPipelineStateInitializer&)
				-> TRefCountPtr<FRHIGraphicsPipelineState> override { return {}; }
			auto RHIGetDefaultContext() -> IRHICommandContext* override
			{
				return &CommandContext;
			}
			auto RHIGetViewportBackBuffer(FRHIViewport*)
				-> TRefCountPtr<FRHITexture> override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&)
				-> TRefCountPtr<FRHIVertexDeclaration> override { return {}; }
			auto RHIIsTextureSupported(
				const FRHITextureCreateDesc&) const -> bool override { return false; }
			auto RHICreateTexture(
				FRHICommandListBase&, const FRHITextureCreateDesc&)
				-> FTextureRHIRef override
			{
				return {};
			}
			auto RHICreateSampler(const FRHISamplerDesc&)
				-> TRefCountPtr<FRHISampler> override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&)
				-> TRefCountPtr<FRHIShader> override { return {}; }
			auto RHICreateBuffer(
				FRHICommandListImmediate&, const FRHIBufferCreateDesc&)
				-> FBufferRHIRef override
			{
				return {};
			}

		private:
			FInitializationCommandContext CommandContext;
			FInitializationObservation& Observation;
			bool bFailInit = true;
		};
	}

	TEST(FRHIInitializationTests,
		ThreadLaunchFailureReleasesUninitializedBackend)
	{
		FInitializationObservation Observation;
		EXPECT_FALSE(RHIInitWithBackendForTests(
			new FFailingDynamicRHI(Observation), true, true,
			FRHIInitializationContext::Headless()));

		EXPECT_EQ(GDynamicRHI, nullptr);
		EXPECT_EQ(Observation.InitCount, 0u);
		EXPECT_EQ(Observation.ShutdownCount, 0u);
		EXPECT_EQ(Observation.DestructionCount, 1u);
	}

	TEST(FRHIInitializationTests,
		BackendOwnsInitFailureCleanupOnRHIThread)
	{
		FInitializationObservation Observation;
		EXPECT_FALSE(RHIInitWithBackendForTests(
			new FFailingDynamicRHI(Observation), true, false,
			FRHIInitializationContext::Headless()));

		EXPECT_EQ(GDynamicRHI, nullptr);
		EXPECT_EQ(Observation.InitCount, 1u);
		EXPECT_EQ(Observation.InitCleanupCount, 1u);
		EXPECT_EQ(Observation.ShutdownCount, 0u);
		EXPECT_EQ(Observation.DestructionCount, 1u);
		EXPECT_TRUE(Observation.bInitOnRHIThread);
		EXPECT_TRUE(Observation.bInitCleanupOnRHIThread);
	}

	TEST(FRHIInitializationTests, UnknownInitFailureCleansUpAndReleasesBackend)
	{
		for (const bool bThreaded : {false, true})
		{
			FInitializationObservation Observation;
			Observation.bUnknownInitFailure = true;
			EXPECT_FALSE(RHIInitWithBackendForTests(
				new FFailingDynamicRHI(Observation), bThreaded, false,
				FRHIInitializationContext::Headless()));
			EXPECT_EQ(Observation.InitCount, 1u);
			EXPECT_EQ(Observation.InitCleanupCount, 1u);
			EXPECT_EQ(Observation.ShutdownCount, 0u);
			EXPECT_EQ(Observation.DestructionCount, 1u);
			EXPECT_EQ(Observation.bInitCleanupOnRHIThread, bThreaded);
			EXPECT_TRUE(Observation.bInitExecutionAllowed);
			EXPECT_TRUE(Observation.bDestructionExecutionAllowed);
			EXPECT_TRUE(IsInRHIExecutionThread());
			EXPECT_EQ(GDynamicRHI, nullptr);
		}
	}

	TEST(FRHIInitializationTests, LifecycleUsesStoppedRenderingThreadAndJoinedRuntimeExecution)
	{
		for (const bool bThreaded : {false, true})
		{
			FInitializationObservation Observation;
			EXPECT_TRUE(IsInRHIExecutionThread());
			ASSERT_TRUE(RHIInitWithBackendForTests(
				new FFailingDynamicRHI(Observation, false), bThreaded, false,
				FRHIInitializationContext::Headless()));
			EXPECT_TRUE(Observation.bInitExecutionAllowed);
			EXPECT_EQ(IsInRHIExecutionThread(), !bThreaded);
			bool bExecuted = false;
			Testing::RunOnRHIRenderingThreadForTests([&] {
				GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
					CheckRHIExecutionThread();
					bExecuted = true;
				});
			});
			EXPECT_TRUE(bExecuted);
			EXPECT_EQ(GRenderingThread, nullptr);
			EXPECT_THROW(Testing::RunOnRHIRenderingThreadForTests([] {
				throw std::runtime_error("intentional rendering-thread failure");
			}), std::runtime_error);
			EXPECT_EQ(GRenderingThread, nullptr);
			EXPECT_EQ(IsInRHIExecutionThread(), !bThreaded);
			RHIExit();
			EXPECT_TRUE(Observation.bShutdownExecutionAllowed);
			EXPECT_TRUE(Observation.bDestructionExecutionAllowed);
			EXPECT_TRUE(IsInRHIExecutionThread());
		}
	}

	TEST(FRHILifecycleDeathTest, LifecycleRequiresRenderingThreadStopAndJoin)
	{
		for (const bool bShutdown : {false, true})
		{
			EXPECT_DEATH(([](bool bShutdown) {
				FInitializationObservation Observation;
				if (bShutdown && !RHIInitWithBackendForTests(
					new FFailingDynamicRHI(Observation, false), false, false,
					FRHIInitializationContext::Headless())) return;
				FBlockedRenderer Runnable;
				std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(
					&Runnable, "LifecycleExclusionTest", 0,
					EThreadPriority::Normal, EThreadRole::RenderingThread));
				if (!Thread) return;
				GRenderingThread = Thread.get();
				Runnable.Ready.Wait();
				if (bShutdown) RHIExit();
				else RHIInitWithBackendForTests(nullptr, false, false,
					FRHIInitializationContext::Headless());
				Runnable.Release.Trigger();
				Thread->WaitForCompletion();
				GRenderingThread = nullptr;
			}(bShutdown)), "Stop and join the rendering thread");
		}
	}

	TEST(FRHILifecycleDeathTest, WorkerCannotEnterLifecycle)
	{
		EXPECT_DEATH(([] {
			std::thread Worker([] {
				RHIInitWithBackendForTests(nullptr, false, false,
					FRHIInitializationContext::Headless());
			});
			Worker.join();
		}()), "requires the registered game thread");
	}

	TEST(FRHIInitializationTests, InitFailureDoesNotInvokeRuntimeShutdown)
	{
		for (const bool bThreaded : {false, true})
		{
			FInitializationObservation Observation;
			Observation.bFailShutdown = true;
			EXPECT_FALSE(RHIInitWithBackendForTests(
				new FFailingDynamicRHI(Observation), bThreaded, false,
				FRHIInitializationContext::Headless()));
			EXPECT_EQ(Observation.InitCleanupCount, 1u);
			EXPECT_EQ(Observation.ShutdownCount, 0u);
			EXPECT_EQ(Observation.DestructionCount, 1u);
			EXPECT_EQ(GDynamicRHI, nullptr);
		}
	}

	TEST(FRHIInitializationTests,
		PresentationContextIsDeliveredToBackendInitialization)
	{
		FInitializationObservation Observation;
		auto* Backend = new FFailingDynamicRHI(Observation);
		void* const WindowHandle = reinterpret_cast<void*>(uintptr_t{0x1234});
		const auto PlatformTarget = std::make_shared<FNativePresentationTarget>(WindowHandle);
		const FRHIPresentationTarget Target{.PlatformTarget = PlatformTarget};
		EXPECT_FALSE(RHIInitWithBackendForTests(
			Backend, false, false,
			FRHIInitializationContext::Presentation(Target)));

		ASSERT_TRUE(Observation.InitializationContext.GetPresentationTarget());
		EXPECT_EQ(Observation.InitializationContext.GetPresentationTarget()
			->GetNativeWindowHandle(), WindowHandle);
		EXPECT_EQ(Observation.InitializationContext.GetPresentationTarget()
			->PlatformTarget, PlatformTarget);
		EXPECT_EQ(Observation.DestructionCount, 1u);
	}

	TEST(FRHIInitializationTests, ExecutionModeDefaultsThreadedAndRetainsInlineOverride)
	{
		EXPECT_EQ(ResolveRHIExecutionMode(nullptr), ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("inline"), ERHIExecutionMode::Inline);
		EXPECT_EQ(ResolveRHIExecutionMode("threaded"),
			ERHIExecutionMode::Threaded);
	}

	TEST(FRHIInitializationTests, BackendSelectionUsesPlatformDefaultAndRequiresExactOverride)
	{
#if defined(__APPLE__)
		EXPECT_EQ(ResolveRHIBackend(nullptr), ERHIBackend::Metal);
#else
		EXPECT_EQ(ResolveRHIBackend(nullptr), ERHIBackend::Vulkan);
#endif
		EXPECT_FALSE(GetActiveRHIBackend());
		EXPECT_EQ(ResolveRHIBackend("vulkan"), ERHIBackend::Vulkan);
		EXPECT_EQ(ResolveRHIBackend("metal"), ERHIBackend::Metal);
		EXPECT_FALSE(ResolveRHIBackend(""));
		EXPECT_FALSE(ResolveRHIBackend("Metal"));
		EXPECT_FALSE(ResolveRHIBackend("unknown"));
	}

	TEST(FRHIInitializationTests, CapabilitySnapshotPublishesOnlyAfterSuccessfulInit)
	{
		FInitializationObservation Observation;
		auto* Backend = new FFailingDynamicRHI(Observation, false);
		EXPECT_EQ(Backend->RHIGetCapabilities(), nullptr);
		Backend->Init(FRHIInitializationContext::Headless());
		const FRHICapabilities* Capabilities = Backend->RHIGetCapabilities();
		ASSERT_NE(Capabilities, nullptr);
		EXPECT_EQ(Capabilities->FeatureLevel, ERHIFeatureLevel::ES3_1);
		EXPECT_EQ(Capabilities->SupportedTextureDimensions,
			ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube);
		EXPECT_EQ(Capabilities->MaxTextureDimension2D, 4096u);
		EXPECT_EQ(Capabilities->MaxComputeWorkGroupCount,
			(std::array<uint32, 3>{65535, 1024, 64}));
		EXPECT_FALSE(Capabilities->bSupportsSynchronization2);
		EXPECT_FALSE(Capabilities->bSupportsGPUTimestamps);
		EXPECT_EQ(Capabilities->GPUTimestampNanosecondsPerTick, 0.0);
		EXPECT_FALSE(Backend->RHICreateGPUTimingQuery());
		EXPECT_EQ(Backend->RHIGetGPUTimingResult(nullptr).State,
			ERHIGPUTimingResultState::Unsupported);
		const FRHIDiagnosticSnapshot Snapshot = Backend->RHIGetDiagnosticSnapshot();
		EXPECT_FALSE(Snapshot.Availability.bRequested);
		EXPECT_EQ(Snapshot.Timing.IntervalCapacity, 0u);
		Backend->RHIResetDiagnosticStatistics();

		Backend->Shutdown();
		EXPECT_TRUE(Observation.bCapabilitiesClearedAtShutdown);
		delete Backend;
		EXPECT_EQ(Observation.DestructionCount, 1u);
	}

	TEST(FRHIInitializationTests, InvalidExecutionModeFallsBackThreaded)
	{
		EXPECT_EQ(ResolveRHIExecutionMode(""), ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("THREADed"),
			ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("unexpected"),
			ERHIExecutionMode::Threaded);
	}
} // namespace Durin
