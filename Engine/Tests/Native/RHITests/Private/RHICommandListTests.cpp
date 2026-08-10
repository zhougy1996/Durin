#include <gtest/gtest.h>

#include "RHI.h"
#include "RHIContext.h"
#include "RHIThread.h"
#include "Threading/ThreadEvent.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FCountedDelete
		{
			void operator()(int* Value) const
			{
				delete Value;
				++*Count;
			}

			int* Count = nullptr;
		};

		class FTrackedRHIResource final : public FRHIResource
		{
		public:
			explicit FTrackedRHIResource(bool& InDestroyed)
				: FRHIResource(ERHIResourceType::Buffer)
				, Destroyed(InDestroyed)
			{
			}

			~FTrackedRHIResource() override
			{
				Destroyed = true;
			}

		private:
			bool& Destroyed;
		};

		class FTestBuffer final : public FRHIBuffer
		{
		public:
			explicit FTestBuffer(uint32 Size)
				: FRHIBuffer(FRHIBufferCreateDesc::Create(
					"TestBuffer", Size, 1, EBufferUsageFlags::VertexBuffer))
			{
			}
		};

		class FTestShader final : public FRHIShader
		{
		public:
			FTestShader(EShaderFrequency Frequency, uint64 Hash)
				: FRHIShader(FRHIShaderDesc(Frequency, FXxHash128{Hash, 0}))
			{
			}
		};

		class FTestVertexDeclaration final : public FRHIVertexDeclaration
		{
		public:
			FTestVertexDeclaration()
			{
				Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 12);
			}
			explicit FTestVertexDeclaration(FVertexDeclarationElementList InElements)
				: Elements(std::move(InElements))
			{
			}

			auto GetElements() const -> const FVertexDeclarationElementList& override
			{
				return Elements;
			}

		private:
			FVertexDeclarationElementList Elements{};
		};

		class FRecordingCommandContext final : public IRHICommandContext
		{
		public:
			auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override
			{
				Operations.emplace_back("BeginFrame");
				ObservedBeginFrameNumber = Args.FrameNumber;
			}
			auto RHISubmitCommands() -> void override
			{
				Operations.emplace_back("SubmitToGPU");
				if (ResourceDestroyed)
				{
					ObservedResourceAliveAtSubmit = !*ResourceDestroyed;
				}
			}
			auto RHIEndFrame() -> void override
			{
				Operations.emplace_back("EndFrame");
				if (bFailEndFrame)
				{
					throw std::runtime_error("intentional EndFrame failure");
				}
				if (ResourceDestroyed)
				{
					ObservedResourceAliveAtEndFrame = !*ResourceDestroyed;
				}
			}
			auto RHIBeginRenderPass(const FRHIRenderPassInfo& Info, FName) -> void override
			{
				Operations.emplace_back("BeginRenderPass");
				ObservedColorTarget = Info.ColorRenderTargets[0];
				std::copy_n(
					Info.ColorClearValues[0].ClearValue.Color,
					4,
					ObservedClearValue.begin());
			}
			auto RHIEndRenderPass() -> void override { Operations.emplace_back("EndRenderPass"); }
			auto RHIBeginDrawingViewport(FRHIViewport*, FRHITexture*) -> void override
			{
				Operations.emplace_back("BeginDrawingViewport");
			}
			auto RHIEndDrawingViewport(
				FRHIViewport*, bool bPresent, bool bLockToVsync) -> void override
			{
				Operations.emplace_back("EndDrawingViewport");
				ObservedPresent = bPresent;
				ObservedLockToVsync = bLockToVsync;
			}
			auto RHISetViewport(float MinX, float, float, float, float, float) -> void override
			{
				Operations.emplace_back("Viewport" + std::to_string(static_cast<int>(MinX)));
			}
			auto RHISetScissor(float MinX, float, float, float) -> void override
			{
				Operations.emplace_back("Scissor" + std::to_string(static_cast<int>(MinX)));
			}
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState&) -> void override
			{
				Operations.emplace_back("PipelineState");
			}
			auto RHIBindVertexBuffer(uint32, FRHIBuffer*, uint32) -> void override
			{
				Operations.emplace_back("VertexBuffer");
			}
			auto RHIBindIndexBuffer(FRHIBuffer*, uint32) -> void override
			{
				Operations.emplace_back("IndexBuffer");
			}
			auto RHITransitionBuffers(
				std::span<const FRHIBufferTransition> Transitions) -> void override
			{
				Operations.emplace_back("TransitionBuffers");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				ObservedBufferTransitions.assign(Transitions.begin(), Transitions.end());
			}
			auto RHITransitionTextures(
				std::span<const FRHITextureTransition> Transitions) -> void override
			{
				Operations.emplace_back("TransitionTextures");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				ObservedTextureTransitions.assign(Transitions.begin(), Transitions.end());
			}
			auto RHICopyBuffer(FRHIBuffer*, FRHIBuffer*,
				std::span<const FRHIBufferCopyRegion> Regions) -> void override
			{
				Operations.emplace_back("CopyBuffer");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				ObservedBufferCopyRegions.assign(Regions.begin(), Regions.end());
			}
			auto RHICopyBufferToTexture(FRHIBuffer*, FRHITexture*,
				std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{
				Operations.emplace_back("CopyBufferToTexture");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				ObservedBufferTextureCopyRegions.assign(Regions.begin(), Regions.end());
			}
			auto RHICopyTextureToBuffer(FRHITexture*, FRHIBuffer*,
				std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{
				Operations.emplace_back("CopyTextureToBuffer");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				ObservedBufferTextureCopyRegions.assign(Regions.begin(), Regions.end());
			}
			auto RHICopyTexture(FRHITexture*, FRHITexture*,
				std::span<const FRHITextureCopyRegion> Regions) -> void override
			{
				Operations.emplace_back("CopyTexture");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				ObservedTextureCopyRegions.assign(Regions.begin(), Regions.end());
			}
			auto RHIWriteBuffer(
				FRHIBuffer* Buffer,
				uint32 Offset,
				std::span<const uint8> Data) -> void override
			{
				Operations.emplace_back("WriteBuffer");
				ObservedBuffer = Buffer;
				ObservedBufferOffset = Offset;
				ObservedBufferData.assign(Data.begin(), Data.end());
			}
			auto RHIInitializeTexture(FRHITexture*) -> void override
			{
				Operations.emplace_back("InitializeTexture");
			}
			auto RHIUpdateTexture2D(
				FRHITexture*, uint32, uint32,
				const FUpdateTextureRegion2D&, uint32,
				std::span<const uint8> SourceData) -> void override
			{
				Operations.emplace_back("UpdateTexture2D");
				ObservedTextureData.assign(SourceData.begin(), SourceData.end());
			}
			auto RHIReadTexture2D(
				FRHITexture*, uint32, uint32,
				std::vector<uint8>& OutData) -> bool override
			{
				Operations.emplace_back("ReadTexture2D");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				OutData = {7, 8, 9};
				return true;
			}
			auto RHIAllocateDynamicUniformBuffer(
				const void*, uint32) -> FRHIUniformBufferRange override
			{
				Operations.emplace_back("AllocateDynamicUniformBuffer");
				OperationThreadRoles.emplace_back(IsInRHIThread());
				return {};
			}
			auto RHIAcquireBackBuffer(FRHITexture*) -> void override
			{
				Operations.emplace_back("AcquireBackBuffer");
				OperationThreadRoles.emplace_back(IsInRHIThread());
			}
			auto RHIBlockUntilGPUIdle() -> void override
			{
				Operations.emplace_back("BlockUntilGPUIdle");
				OperationThreadRoles.emplace_back(IsInRHIThread());
			}
			auto RHIPushConstants(EShaderStageFlags, uint32, uint32 Size, const void* Data) -> void override
			{
				Operations.emplace_back("PushConstants");
				const auto* Bytes = static_cast<const uint8*>(Data);
				ObservedPushConstants.assign(Bytes, Bytes + Size);
			}
			auto RHISetShaderParameters(
				FRHIShader* InShader,
				const std::span<FRHIShaderParameterResource>& InParameters) -> void override
			{
				Operations.emplace_back("ShaderParameters");
				ObservedShader = InShader;
				ObservedShaderParameters.assign(InParameters.begin(), InParameters.end());
			}
			auto RHIDraw(const FRHIDrawArguments& Arguments) -> void override
			{
				Operations.emplace_back("Draw" + std::to_string(Arguments.VertexCount));
				ObservedDrawArguments = Arguments;
			}
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void override
			{
				Operations.emplace_back("Draw" + std::to_string(Arguments.IndexCount));
				ObservedDrawIndexedArguments = Arguments;
			}

			std::vector<std::string> Operations;
			std::optional<uint64> ObservedBeginFrameNumber;
			std::vector<bool> OperationThreadRoles;
			FRHITexture* ObservedColorTarget = nullptr;
			std::array<float, 4> ObservedClearValue{};
			std::vector<uint8> ObservedPushConstants;
			FRHIShader* ObservedShader = nullptr;
			std::vector<FRHIShaderParameterResource> ObservedShaderParameters;
			std::optional<FRHIDrawArguments> ObservedDrawArguments;
			std::optional<FRHIDrawIndexedArguments> ObservedDrawIndexedArguments;
			FRHIBuffer* ObservedBuffer = nullptr;
			uint32 ObservedBufferOffset = 0;
			std::vector<uint8> ObservedBufferData;
			std::vector<uint8> ObservedTextureData;
			std::vector<FRHIBufferTransition> ObservedBufferTransitions;
			std::vector<FRHITextureTransition> ObservedTextureTransitions;
			std::vector<FRHIBufferCopyRegion> ObservedBufferCopyRegions;
			std::vector<FRHIBufferTextureCopyRegion> ObservedBufferTextureCopyRegions;
			std::vector<FRHITextureCopyRegion> ObservedTextureCopyRegions;
			bool* ResourceDestroyed = nullptr;
			bool ObservedResourceAliveAtSubmit = false;
			bool ObservedResourceAliveAtEndFrame = false;
			bool ObservedPresent = false;
			bool ObservedLockToVsync = false;
			bool bFailEndFrame = false;
		};
	}

	TEST(FRHICommandListTests, FinishRecordingDoesNotSubmitOrExecute)
	{
		FRHICommandListExecutor Executor;
		FRHICommandList CommandList;
		bool bExecuted = false;
		CommandList.EnqueueLambda([&bExecuted]() { bExecuted = true; });

		CommandList.FinishRecording();

		EXPECT_TRUE(CommandList.IsFinished());
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(Executor.GetLastSubmittedSerial(), 0u);
	}

	TEST(FRHICommandListTests, ImmediateInsertionPreservesPrimaryTimelineOrder)
	{
		FRHICommandListExecutor Executor;
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		std::vector<char> ReplayOrder;

		Immediate.EnqueueLambda([&ReplayOrder]() { ReplayOrder.push_back('A'); });
		FRHICommandList Parallel;
		Parallel.EnqueueLambda([&ReplayOrder]() { ReplayOrder.push_back('P'); });
		Parallel.FinishRecording();
		Immediate.QueueCommandList(std::move(Parallel));
		Immediate.EnqueueLambda([&ReplayOrder]() { ReplayOrder.push_back('B'); });

		const uint64 Serial = Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(ReplayOrder, (std::vector<char>{'A', 'P', 'B'}));
		EXPECT_EQ(Serial, 1u);
		EXPECT_EQ(Executor.GetCompletedSerial(), Serial);
	}

	TEST(FRHICommandListTests, AdditionalListsAppendInArgumentOrder)
	{
		FRHICommandListExecutor Executor;
		std::vector<int> ReplayOrder;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[&ReplayOrder]() { ReplayOrder.push_back(0); });

		FRHICommandList First;
		First.EnqueueLambda([&ReplayOrder]() { ReplayOrder.push_back(1); });
		First.FinishRecording();
		FRHICommandList Second;
		Second.EnqueueLambda([&ReplayOrder]() { ReplayOrder.push_back(2); });
		Second.FinishRecording();

		Executor.Submit({&First, &Second}, ERHISubmitFlags::None);

		EXPECT_EQ(ReplayOrder, (std::vector<int>{0, 1, 2}));
	}

	TEST(FRHICommandListTests, OwnedCallableSurvivesSourceMutation)
	{
		FRHICommandListExecutor Executor;
		std::vector<uint8> Source{1, 2, 3, 4};
		std::vector<uint8> Replayed;
		std::vector<uint8> Owned = Source;
		const size_t OwnedPayloadBytes = Owned.capacity() * sizeof(Owned.front());
		Executor.GetImmediateCommandList().EnqueueLambda(
			[Owned = std::move(Owned), &Replayed]() { Replayed = Owned; },
			OwnedPayloadBytes);
		std::fill(Source.begin(), Source.end(), 9);

		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Replayed, (std::vector<uint8>{1, 2, 3, 4}));
	}

	TEST(FRHICommandListTests, BatchRetainsResourcesUntilReplayCompletes)
	{
		FRHICommandListExecutor Executor;
		bool bDestroyed = false;
		bool bObservedDuringReplay = false;
		TRefCountPtr<FTrackedRHIResource> Resource =
			MakeRefCount<FTrackedRHIResource>(bDestroyed);
		Executor.GetImmediateCommandList().EnqueueLambda(
			[Owned = Resource, &bObservedDuringReplay]() {
				bObservedDuringReplay = Owned.GetReference() != nullptr;
			}, 0);
		Resource = nullptr;

		EXPECT_FALSE(bDestroyed);
		Executor.Submit({}, ERHISubmitFlags::DeleteResources);

		EXPECT_TRUE(bObservedDuringReplay);
		EXPECT_TRUE(bDestroyed);
	}

	TEST(FRHICommandListTests, FinalReleasePermanentlyClosesResourceLifetime)
	{
		bool bDestroyed = false;
		auto* Resource = new FTrackedRHIResource(bDestroyed);
		EXPECT_EQ(Resource->AddRef(), 1u);
		EXPECT_EQ(Resource->Release(), 0u);
		EXPECT_EQ(Resource->GetRefCount(), 0u);

		EXPECT_DEATH_IF_SUPPORTED(Resource->AddRef(), "");
		RHIFlushDeferredResources();

		EXPECT_TRUE(bDestroyed);
		EXPECT_EQ(FRHIResource::GetNumPendingDeletes(), 0u);
	}

	TEST(FRHICommandListTests, RejectsQueueBeforeFinishAndDoubleAdmission)
	{
		FRHICommandListExecutor Executor;
		FRHICommandList CommandList;

		EXPECT_FALSE(Executor.GetImmediateCommandList().TryQueueCommandList(
			std::move(CommandList)));
		CommandList.FinishRecording();
		EXPECT_TRUE(Executor.GetImmediateCommandList().TryQueueCommandList(
			std::move(CommandList)));
		EXPECT_FALSE(Executor.GetImmediateCommandList().TryQueueCommandList(
			std::move(CommandList)));
	}

	TEST(FRHICommandListTests, FinishedListsMoveWithoutCopyingTheirCommands)
	{
		FRHICommandListExecutor Executor;
		bool bExecuted = false;
		FRHICommandList Source;
		Source.EnqueueLambda([&bExecuted]() { bExecuted = true; });
		Source.FinishRecording();

		FRHICommandList Destination(std::move(Source));
		EXPECT_FALSE(Source.IsRecording());
		EXPECT_TRUE(Destination.IsFinished());
		Executor.GetImmediateCommandList().QueueCommandList(
			std::move(Destination));
		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_TRUE(bExecuted);
	}

	TEST(FRHICommandListTests, RejectsRecordingAfterFinish)
	{
#if DO_CHECK
		FRHICommandList CommandList;
		CommandList.FinishRecording();

		EXPECT_DEATH(CommandList.EnqueueLambda([]() {}), "");
#else
		GTEST_SKIP() << "Ordinary check contracts are disabled in Shipping.";
#endif
	}

	TEST(FRHICommandListTests, DestroysOwnedCommandsOnceAfterReplay)
	{
		FRHICommandListExecutor Executor;
		int DestructionCount = 0;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[Owned = std::unique_ptr<int, FCountedDelete>(
				new int(1), FCountedDelete{&DestructionCount})]() {},
			sizeof(int));

		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(DestructionCount, 1);
	}

	TEST(FRHICommandListTests, RejectedListReleasesOwnedCommandsOnce)
	{
		FRHICommandListExecutor Executor;
		int DestructionCount = 0;
		{
			FRHICommandList Rejected;
			Rejected.EnqueueLambda(
				[Owned = std::unique_ptr<int, FCountedDelete>(
					new int(1), FCountedDelete{&DestructionCount})]() {},
				sizeof(int));
			EXPECT_FALSE(
				Executor.GetImmediateCommandList().TryQueueCommandList(
					std::move(Rejected)));
			EXPECT_EQ(DestructionCount, 0);
		}

		EXPECT_EQ(DestructionCount, 1);
	}

	TEST(FRHICommandListTests, FenceTargetsTheLatestCompletedSubmission)
	{
		FRHICommandListExecutor Executor;
		Executor.GetImmediateCommandList().EnqueueLambda([]() {});
		const uint64 Serial = Executor.Submit({}, ERHISubmitFlags::None);
		const FRHICommandListFence Fence = Executor.CreateFence();

		EXPECT_EQ(Fence.GetTargetSerial(), Serial);
		EXPECT_TRUE(Fence.IsComplete());
		Fence.Wait();
	}

	TEST(FRHICommandListTests, FlushTypesDistinguishProducerDispatchAndSerialWait)
	{
		FRHICommandListExecutor Executor;
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		bool bExecuted = false;
		Immediate.EnqueueLambda([&bExecuted]() { bExecuted = true; });

		Immediate.ImmediateFlush(EImmediateFlushType::WaitForOutstandingTasksOnly);
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(Executor.GetLastSubmittedSerial(), 0u);
		EXPECT_EQ(Executor.GetStats().WaitCount, 0u);

		Immediate.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
		EXPECT_TRUE(bExecuted);
		EXPECT_EQ(Executor.GetLastSubmittedSerial(), 1u);
		EXPECT_EQ(Executor.GetStats().WaitCount, 0u);

		Immediate.EnqueueLambda([]() {});
		Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		EXPECT_EQ(Executor.GetLastSubmittedSerial(), 2u);
		EXPECT_EQ(Executor.GetCompletedSerial(), 2u);
		EXPECT_EQ(Executor.GetStats().WaitCount, 1u);

		Immediate.EnqueueLambda([]() {});
		Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::FlushRHIThread);
		EXPECT_EQ(Executor.GetLastSubmittedSerial(), 3u);
		EXPECT_EQ(Executor.GetCompletedSerial(), 3u);
		EXPECT_EQ(Executor.GetStats().WaitCount, 2u);
	}

	TEST(FRHICommandListTests, ThreadedDispatchReturnsBeforeReplayAndFenceWaits)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		std::promise<void> ReleaseReplay;
		std::shared_future<void> ReplayGate = ReleaseReplay.get_future().share();
		std::atomic<bool> bReplayStarted = false;
		std::atomic<bool> bExecutedOnRHIThread = false;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[ReplayGate, &bReplayStarted, &bExecutedOnRHIThread]() {
				bReplayStarted.store(true, std::memory_order_release);
				bExecutedOnRHIThread.store(IsInRHIThread(), std::memory_order_release);
				ReplayGate.wait();
			}, 0);

		const uint64 Serial = Executor.Submit({}, ERHISubmitFlags::None);
		while (!bReplayStarted.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		EXPECT_LT(Executor.GetCompletedSerial(), Serial);
		EXPECT_EQ(Executor.GetStats().Mode, ERHICommandListExecutorMode::Threaded);

		ReleaseReplay.set_value();
		Executor.CreateFence().Wait();
		EXPECT_EQ(Executor.GetCompletedSerial(), Serial);
		EXPECT_TRUE(bExecutedOnRHIThread.load(std::memory_order_acquire));
	}

	TEST(FRHICommandListTests,
		ThreadedRapidDispatchAndFlushStressCompletesEverySubmission)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		constexpr uint32 SubmissionCount = 256;
		std::atomic<uint32> NextExpectedIndex = 0;
		std::atomic<bool> bObservedFIFO = true;

		for (uint32 Index = 0; Index < SubmissionCount; ++Index)
		{
			Immediate.EnqueueLambda([Index, &NextExpectedIndex, &bObservedFIFO]() {
				const uint32 ObservedIndex = NextExpectedIndex.fetch_add(
					1, std::memory_order_relaxed);
				if (ObservedIndex != Index)
				{
					bObservedFIFO.store(false, std::memory_order_relaxed);
				}
			});
			Immediate.ImmediateFlush(
				Index % 8 == 7
					? EImmediateFlushType::FlushRHIThread
					: EImmediateFlushType::DispatchToRHIThread);
		}
		Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

		const FRHICommandListExecutorStats Stats = Executor.GetStats();
		EXPECT_TRUE(bObservedFIFO.load(std::memory_order_relaxed));
		EXPECT_EQ(NextExpectedIndex.load(std::memory_order_relaxed), SubmissionCount);
		EXPECT_EQ(Stats.RecordedCommandCount, SubmissionCount);
		EXPECT_EQ(Stats.SubmissionGroupCount, SubmissionCount);
		EXPECT_EQ(Stats.RejectedSubmissionCount, 0u);
		EXPECT_EQ(Stats.PendingBatchCount, 0u);
		EXPECT_EQ(Stats.LastSubmittedSerial, Stats.CompletedSerial);
		EXPECT_GT(Stats.PeakQueueEntryCount, 0u);
		EXPECT_LE(Stats.PeakQueueEntryCount, 8u);
	}

	TEST(FRHICommandListTests, ThreadedReplayMatchesInlineOrderedEvents)
	{
		FRecordingCommandContext InlineContext;
		FRHICommandListExecutor InlineExecutor(InlineContext);
		InlineExecutor.GetImmediateCommandList().EnqueueLambda(
			[&InlineContext]() { InlineContext.Operations.emplace_back("Replay"); });
		InlineExecutor.GetImmediateCommandList().SwitchPipeline(
			ERHIPipeline::Graphics);
		InlineExecutor.GetImmediateCommandList().BeginDrawingViewport(
			nullptr, nullptr);
		InlineExecutor.GetImmediateCommandList().EndDrawingViewport(
			nullptr, true, true);
		FRHICommandList InlineAdditional;
		InlineAdditional.EnqueueLambda(
			[&InlineContext]() { InlineContext.Operations.emplace_back("Additional"); });
		InlineAdditional.FinishRecording();
		InlineExecutor.Submit(
			{&InlineAdditional}, ERHISubmitFlags::BeginFrame
				| ERHISubmitFlags::SubmitToGPU | ERHISubmitFlags::EndFrame);

		FRecordingCommandContext ThreadedContext;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor ThreadedExecutor(ThreadedContext, RHIThread);
		ThreadedExecutor.GetImmediateCommandList().EnqueueLambda(
			[&ThreadedContext]() { ThreadedContext.Operations.emplace_back("Replay"); });
		ThreadedExecutor.GetImmediateCommandList().SwitchPipeline(
			ERHIPipeline::Graphics);
		ThreadedExecutor.GetImmediateCommandList().BeginDrawingViewport(
			nullptr, nullptr);
		ThreadedExecutor.GetImmediateCommandList().EndDrawingViewport(
			nullptr, true, true);
		FRHICommandList ThreadedAdditional;
		ThreadedAdditional.EnqueueLambda(
			[&ThreadedContext]() { ThreadedContext.Operations.emplace_back("Additional"); });
		ThreadedAdditional.FinishRecording();
		const uint64 Serial = ThreadedExecutor.Submit(
			{&ThreadedAdditional}, ERHISubmitFlags::BeginFrame
				| ERHISubmitFlags::SubmitToGPU | ERHISubmitFlags::EndFrame);
		ThreadedExecutor.CreateFence().Wait();

		EXPECT_EQ(ThreadedExecutor.GetCompletedSerial(), Serial);
		EXPECT_EQ(ThreadedContext.Operations, InlineContext.Operations);
		EXPECT_EQ(InlineContext.ObservedBeginFrameNumber, 0);
		EXPECT_EQ(ThreadedContext.ObservedBeginFrameNumber, 0);
		EXPECT_EQ(InlineExecutor.GetFrameNumber(), 1u);
		EXPECT_EQ(ThreadedExecutor.GetFrameNumber(), 1u);
		EXPECT_TRUE(InlineContext.ObservedPresent);
		EXPECT_TRUE(InlineContext.ObservedLockToVsync);
		EXPECT_TRUE(ThreadedContext.ObservedPresent);
		EXPECT_TRUE(ThreadedContext.ObservedLockToVsync);
		EXPECT_EQ(ThreadedContext.Operations, (std::vector<std::string>{
			"BeginFrame", "Replay", "BeginDrawingViewport", "EndDrawingViewport",
			"Additional", "SubmitToGPU", "EndFrame"}));
	}

	TEST(FRHICommandListTests, ThreadedImmediateOperationsStayOnRHIThread)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>();
		const std::array<uint8, 4> UniformData{1, 2, 3, 4};
		std::vector<uint8> Readback;

		Immediate.AcquireBackBuffer(Texture.GetReference());
		Immediate.AllocateDynamicUniformBuffer(
			UniformData.data(), static_cast<uint32>(UniformData.size()));
		EXPECT_TRUE(Immediate.ReadTexture2D(
			Texture.GetReference(), 0, 0, Readback));
		Immediate.BlockUntilGPUIdle();

		EXPECT_EQ(Readback, (std::vector<uint8>{7, 8, 9}));
		EXPECT_EQ(Context.Operations, (std::vector<std::string>{
			"AllocateDynamicUniformBuffer", "AcquireBackBuffer",
			"ReadTexture2D", "BlockUntilGPUIdle"}));
		EXPECT_EQ(Context.OperationThreadRoles,
			(std::vector<bool>{true, true, true, true}));
	}

	TEST(FRHICommandListTests, EmptySubmitFlushFlagWaitsForOutstandingSerial)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		std::promise<void> ReleaseReplay;
		std::shared_future<void> ReplayGate = ReleaseReplay.get_future().share();
		std::atomic<bool> bReplayStarted = false;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[ReplayGate, &bReplayStarted]() {
				bReplayStarted.store(true, std::memory_order_release);
				ReplayGate.wait();
			}, 0);
		const uint64 Serial = Executor.Submit({}, ERHISubmitFlags::None);
		while (!bReplayStarted.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}

		auto Flush = std::async(std::launch::async, [&Executor]() {
			return Executor.Submit({}, ERHISubmitFlags::FlushRHIThread);
		});
		EXPECT_EQ(Flush.wait_for(std::chrono::milliseconds(0)),
			std::future_status::timeout);
		ReleaseReplay.set_value();
		EXPECT_EQ(Flush.get(), Serial);
		EXPECT_EQ(Executor.GetCompletedSerial(), Serial);
		EXPECT_EQ(Executor.GetStats().WaitCount, 1u);
	}

	TEST(FRHICommandListTests, OrderedEventsReleaseBatchesBeforeDeferredDeletion)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		bool bDestroyed = false;
		Context.ResourceDestroyed = &bDestroyed;
		TRefCountPtr<FTrackedRHIResource> Resource =
			MakeRefCount<FTrackedRHIResource>(bDestroyed);
		Executor.GetImmediateCommandList().EnqueueLambda(
			[Owned = Resource, &Context]() {
				Context.Operations.emplace_back("Replay");
				EXPECT_NE(Owned.GetReference(), nullptr);
			}, 0);
		Resource = nullptr;

		const uint64 Serial = Executor.Submit({},
			ERHISubmitFlags::SubmitToGPU
				| ERHISubmitFlags::EndFrame
				| ERHISubmitFlags::DeleteResources);

		EXPECT_EQ(Context.Operations, (std::vector<std::string>{
			"Replay", "SubmitToGPU", "EndFrame"}));
		EXPECT_TRUE(Context.ObservedResourceAliveAtSubmit);
		EXPECT_TRUE(Context.ObservedResourceAliveAtEndFrame);
		EXPECT_TRUE(bDestroyed);
		EXPECT_EQ(Executor.GetCompletedSerial(), Serial);
	}

	TEST(FRHICommandListTests, ExecutorStatsCoverBatchesPayloadsAndRejections)
	{
		FRHICommandListExecutor Executor;
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		Immediate.EnqueueLambda([]() {});
		FRHICommandList Inserted;
		Inserted.EnqueueLambda([]() {});
		EXPECT_FALSE(Immediate.TryQueueCommandList(std::move(Inserted)));
		Inserted.FinishRecording();
		EXPECT_TRUE(Immediate.TryQueueCommandList(std::move(Inserted)));
		Immediate.EnqueueLambda([]() {});

		Executor.Submit({}, ERHISubmitFlags::None);
		const FRHICommandListExecutorStats Stats = Executor.GetStats();
		EXPECT_EQ(Stats.RecordedCommandCount, 3u);
		EXPECT_GT(Stats.RecordedPayloadBytes, 0u);
		EXPECT_EQ(Stats.SubmittedBatchCount, 3u);
		EXPECT_EQ(Stats.SubmissionGroupCount, 1u);
		EXPECT_EQ(Stats.RejectedSubmissionCount, 1u);
		EXPECT_EQ(Stats.PendingBatchCount, 0u);
	}

	TEST(FRHICommandListTests,
		OwnedBufferAndTexturePayloadBytesRejectWithoutLosingBatches)
	{
		FRecordingCommandContext Context;
		FRHIThreadQueueLimits Limits;
		Limits.MaxPayloadBytes = 1024;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start(Limits));
		FRHICommandListExecutor Executor(Context, RHIThread);

		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(2048);
		std::vector<uint8> BufferSource(2048, 7);
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
			"PayloadAccountingTexture", 32, 32, EPixelFormat::RGBA8_UNORM);
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>(Desc);
		std::vector<uint8> TextureSource(32 * 32 * 4, 9);
		FUpdateTextureRegion2D Region(0, 0, 0, 0, 32, 32);

		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		Immediate.WriteBuffer(
			Buffer.GetReference(), BufferSource.data(),
			static_cast<uint32>(BufferSource.size()), 0);
		Immediate.UpdateTexture2D(
			Texture.GetReference(), 0, 0, Region, 32 * 4,
			TextureSource.data());

		const FRHICommandListSubmission Rejected =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		EXPECT_EQ(Rejected.Result, ERHICommandListSubmitResult::Oversized);
		EXPECT_EQ(Rejected.Serial, 0u);
		const FRHICommandListExecutorStats RejectedStats = Executor.GetStats();
		EXPECT_EQ(RejectedStats.RecordedCommandCount, 0u);
		EXPECT_EQ(RejectedStats.RecordedPayloadBytes, 0u);
		EXPECT_EQ(RejectedStats.SubmittedBatchCount, 0u);
		EXPECT_EQ(RejectedStats.SubmissionGroupCount, 0u);
		EXPECT_EQ(RejectedStats.RejectedSubmissionCount, 1u);
		EXPECT_EQ(RejectedStats.PendingBatchCount, 1u);
		EXPECT_GE(
			RejectedStats.PendingPayloadBytes,
			BufferSource.size() + TextureSource.size());

		Executor.SetInlineMode();
		const FRHICommandListSubmission Retried =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		EXPECT_TRUE(Retried.IsAccepted());
		EXPECT_EQ(Retried.Serial, 1u);
		EXPECT_EQ(Context.ObservedBufferData, BufferSource);
		EXPECT_EQ(Context.ObservedTextureData, TextureSource);
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests,
		ClosingThreadRejectionPreservesCommandsForRetry)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		bool bExecuted = false;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[&bExecuted]() { bExecuted = true; });
		RHIThread.BeginDrain();

		const FRHICommandListSubmission Rejected =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		EXPECT_TRUE(
			Rejected.Result == ERHICommandListSubmitResult::ThreadDraining
			|| Rejected.Result == ERHICommandListSubmitResult::ThreadStopped);
		EXPECT_EQ(Rejected.Serial, 0u);
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(Executor.GetStats().PendingBatchCount, 1u);

		Executor.SetInlineMode();
		const FRHICommandListSubmission Retried =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		EXPECT_TRUE(Retried.IsAccepted());
		EXPECT_TRUE(bExecuted);
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests,
		FallibleSynchronousOperationFailureIsRecoverableInline)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		const FRHIFallibleOperationResult Failure =
			Executor.ExecuteFallibleSynchronousOperation(false, []() {
				throw std::runtime_error("intentional creation failure");
			});

		bool bLaterWorkExecuted = false;
		const FRHIFallibleOperationResult Success =
			Executor.ExecuteFallibleSynchronousOperation(false,
				[&bLaterWorkExecuted]() { bLaterWorkExecuted = true; });

		EXPECT_FALSE(Failure.IsSuccess());
		EXPECT_EQ(Failure.Diagnostic, "intentional creation failure");
		EXPECT_TRUE(Success.IsSuccess());
		EXPECT_TRUE(Success.Diagnostic.empty());
		EXPECT_TRUE(bLaterWorkExecuted);
		EXPECT_EQ(Executor.GetStats().SynchronousOperationCount, 2u);
	}

	TEST(FRHICommandListTests,
		FallibleSynchronousOperationFailureIsRecoverableThreaded)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);

		const FRHIFallibleOperationResult Failure =
			Executor.ExecuteFallibleSynchronousOperation(false, []() {
				throw 7;
			});
		bool bLaterWorkExecutedOnRHIThread = false;
		const FRHIFallibleOperationResult Success =
			Executor.ExecuteFallibleSynchronousOperation(false,
				[&bLaterWorkExecutedOnRHIThread]() {
					bLaterWorkExecutedOnRHIThread = IsInRHIThread();
				});

		EXPECT_FALSE(Failure.IsSuccess());
		EXPECT_EQ(Failure.Diagnostic,
			"Fallible RHI operation failed with an unknown exception.");
		EXPECT_TRUE(Success.IsSuccess());
		EXPECT_TRUE(bLaterWorkExecutedOnRHIThread);
		const FRHICommandListExecutorStats Stats = Executor.GetStats();
		EXPECT_EQ(Stats.SynchronousOperationCount, 2u);
		EXPECT_EQ(Stats.LastSubmittedSerial, Stats.CompletedSerial);
		EXPECT_TRUE(RHIThread.GetStats().FailureDiagnostic.empty());

		Executor.SetInlineMode();
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests,
		FailedThreadRejectionPreservesCommandsForRetry)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		FRHIThreadWork FailingWork;
		FailingWork.Execute = []() {
			return FRHIThreadWorkResult::Failure("intentional consumer failure");
		};
		const FRHIThreadSubmission FailureSubmission =
			RHIThread.Enqueue(FailingWork);
		ASSERT_TRUE(FailureSubmission.IsAccepted());
		EXPECT_EQ(
			RHIThread.WaitForSerial(FailureSubmission.Serial),
			ERHIThreadWaitResult::Failed);

		bool bExecuted = false;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[&bExecuted]() { bExecuted = true; });
		const FRHICommandListSubmission Rejected =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		EXPECT_EQ(Rejected.Result, ERHICommandListSubmitResult::ThreadFailed);
		EXPECT_EQ(Rejected.Serial, 0u);
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(Executor.GetStats().PendingBatchCount, 1u);

		Executor.SetInlineMode();
		const FRHICommandListSubmission Retried =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		EXPECT_TRUE(Retried.IsAccepted());
		EXPECT_TRUE(bExecuted);
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests, EmptySubmitDoesNotCreateASerial)
	{
		FRHICommandListExecutor Executor;

		EXPECT_EQ(Executor.Submit({}, ERHISubmitFlags::None), 0u);
		EXPECT_EQ(Executor.GetCompletedSerial(), 0u);
		EXPECT_EQ(Executor.GetFrameNumber(), 0u);
	}

	TEST(FRHICommandListTests,
		EmptySubmitAndFenceCaptureQueueAuthoritativeSerial)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		FThreadEvent WorkStarted;
		FThreadEvent ReleaseWork;

		FRHIThreadWork LifecycleWork;
		LifecycleWork.Execute = [&]() {
			WorkStarted.Trigger();
			ReleaseWork.Wait();
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission LifecycleSubmission =
			RHIThread.Enqueue(LifecycleWork);
		ASSERT_TRUE(LifecycleSubmission.IsAccepted());
		if (!WorkStarted.WaitFor(1.0))
		{
			ReleaseWork.Trigger();
			Executor.SetInlineMode();
			RHIThread.Stop();
			FAIL() << "lifecycle work did not start";
		}

		const FRHICommandListSubmission EmptySubmission =
			Executor.TrySubmit({}, ERHISubmitFlags::None);
		if (!EmptySubmission.IsAccepted())
		{
			ReleaseWork.Trigger();
			Executor.SetInlineMode();
			RHIThread.Stop();
			FAIL() << "empty submission was rejected";
		}
		EXPECT_EQ(LifecycleSubmission.Serial, EmptySubmission.Serial);
		const FRHICommandListFence Fence = Executor.CreateFence();
		EXPECT_EQ(LifecycleSubmission.Serial, Fence.GetTargetSerial());

		ReleaseWork.Trigger();
		Fence.Wait();
		Executor.SetInlineMode();
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests, ExecutorOwnsFrameNumberAcrossEmptyFrames)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);

		EXPECT_EQ(Executor.GetFrameNumber(), 0u);
		Executor.Submit({}, ERHISubmitFlags::BeginFrame);
		EXPECT_EQ(Context.ObservedBeginFrameNumber, 0);
		EXPECT_EQ(Executor.GetFrameNumber(), 0u);

		Executor.Submit({}, ERHISubmitFlags::EndFrame);
		EXPECT_EQ(Executor.GetFrameNumber(), 1u);
		Executor.Submit({}, ERHISubmitFlags::BeginFrame | ERHISubmitFlags::EndFrame);
		EXPECT_EQ(Context.ObservedBeginFrameNumber, 1);
		EXPECT_EQ(Executor.GetFrameNumber(), 2u);
	}

	TEST(FRHICommandListTests, FailedEndFrameDoesNotAdvanceFrameNumber)
	{
		FRecordingCommandContext Context;
		Context.bFailEndFrame = true;
		FRHICommandListExecutor Executor(Context);

		EXPECT_THROW(
			Executor.Submit({}, ERHISubmitFlags::EndFrame),
			std::runtime_error);
		EXPECT_EQ(Executor.GetFrameNumber(), 0u);
		EXPECT_EQ(Executor.GetCompletedSerial(), 0u);
	}

	TEST(FRHICommandListTests, RejectedThreadedEndFrameDoesNotAdvanceFrameNumber)
	{
		FRecordingCommandContext Context;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor Executor(Context, RHIThread);
		RHIThread.BeginDrain();

		const FRHICommandListSubmission Submission =
			Executor.TrySubmit({}, ERHISubmitFlags::EndFrame);
		EXPECT_FALSE(Submission.IsAccepted());
		EXPECT_EQ(Executor.GetFrameNumber(), 0u);

		Executor.SetInlineMode();
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests, FrameNumberSurvivesExecutorModeTransitions)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		Executor.Submit({}, ERHISubmitFlags::EndFrame);
		EXPECT_EQ(Executor.GetFrameNumber(), 1u);

		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		Executor.SetThreadedMode(RHIThread);
		Executor.Submit(
			{}, ERHISubmitFlags::BeginFrame | ERHISubmitFlags::EndFrame);
		Executor.CreateFence().Wait();
		EXPECT_EQ(Context.ObservedBeginFrameNumber, 1);
		EXPECT_EQ(Executor.GetFrameNumber(), 2u);

		Executor.SetInlineMode();
		EXPECT_EQ(Executor.GetFrameNumber(), 2u);
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests, EmptyRegularListDoesNotCreateASerial)
	{
		FRHICommandListExecutor Executor;
		FRHICommandList Empty;
		Empty.FinishRecording();
		EXPECT_TRUE(Executor.GetImmediateCommandList().TryQueueCommandList(
			std::move(Empty)));

		EXPECT_EQ(Executor.Submit({}, ERHISubmitFlags::None), 0u);
		EXPECT_EQ(Executor.GetCompletedSerial(), 0u);
	}

	TEST(FRHICommandListTests, GraphicsCommandsUseReplayContextInTimelineOrder)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		Immediate.SwitchPipeline(ERHIPipeline::Graphics);
		Immediate.SetViewport(1, 2, 0, 3, 4, 1);

		FRHICommandList Inserted;
		Inserted.SwitchPipeline(ERHIPipeline::Graphics);
		Inserted.SetScissor(2, 0, 8, 8);
		Inserted.SwitchPipeline(ERHIPipeline::None);
		Inserted.FinishRecording();
		Immediate.QueueCommandList(std::move(Inserted));
		Immediate.DrawIndexed(3, 0, 0);

		EXPECT_TRUE(Context.Operations.empty());
		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.Operations,
			(std::vector<std::string>{"Viewport1", "Scissor2", "Draw3"}));
	}

	TEST(FRHICommandListTests, AdditionalGraphicsListsAppendInArgumentOrder)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		Executor.GetImmediateCommandList().SwitchPipeline(ERHIPipeline::Graphics);
		Executor.GetImmediateCommandList().SetViewport(0, 0, 0, 1, 1, 1);

		FRHICommandList First;
		First.SwitchPipeline(ERHIPipeline::Graphics);
		First.SetScissor(1, 0, 1, 1);
		First.FinishRecording();
		FRHICommandList Second;
		Second.SwitchPipeline(ERHIPipeline::Graphics);
		Second.DrawIndexed(2, 0, 0);
		Second.FinishRecording();

		Executor.Submit({&First, &Second}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.Operations,
			(std::vector<std::string>{"Viewport0", "Scissor1", "Draw2"}));
	}

	TEST(FRHICommandListTests, CompleteDrawArgumentsReplayWithoutLoss)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		Immediate.SwitchPipeline(ERHIPipeline::Graphics);
		const size_t BeforeNoOps = Immediate.GetNumRecordedCommands();
		Immediate.Draw({.VertexCount = 0, .InstanceCount = 4});
		Immediate.DrawIndexed({.IndexCount = 4, .InstanceCount = 0});
		EXPECT_EQ(Immediate.GetNumRecordedCommands(), BeforeNoOps);

		const FRHIDrawArguments DrawArguments{7, 3, 2, 5};
		const FRHIDrawIndexedArguments IndexedArguments{9, 4, 6, -2, 8};
		Immediate.Draw(DrawArguments);
		Immediate.DrawIndexed(IndexedArguments);
		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.ObservedDrawArguments, DrawArguments);
		EXPECT_EQ(Context.ObservedDrawIndexedArguments, IndexedArguments);
	}

	TEST(FRHICommandListTests, GraphicsPayloadsAreOwnedUntilReplay)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D("RecordedTexture", 1, 1, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable));
		TRefCountPtr<FRHIBuffer> Buffer = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create(
				"RecordedBuffer", 64, 16, EBufferUsageFlags::UniformBuffer));
		TRefCountPtr<FRHIShader> Shader = MakeRefCount<FRHIShader>(
			FRHIShaderDesc(EShaderFrequency::Vertex, FXxHash128{}));
		FGraphicsPipelineStateRHIRef PipelineState =
			MakeRefCount<FRHIGraphicsPipelineState>();

		FRHIRenderPassInfo PassInfo;
		PassInfo.ColorRenderTargets[0] = Texture.GetReference();
		PassInfo.ColorClearValues[0] = FClearValueBinding(1, 2, 3, 4);
		std::vector<uint8> PushBytes{1, 2, 3, 4};
		std::vector<FRHIShaderParameterResource> Parameters{
			{.Resource = Buffer.GetReference(), .SetIndex = 1, .BindingIndex = 2,
				.ArrayElement = 0, .Type = ERHIBindingType::UniformBuffer,
				.Offset = 16, .Size = 32},
			{.Resource = Buffer.GetReference(), .SetIndex = 1, .BindingIndex = 2,
				.ArrayElement = 1, .Type = ERHIBindingType::UniformBuffer,
				.Offset = 16, .Size = 32}};

		Immediate.SwitchPipeline(ERHIPipeline::Graphics);
		Immediate.SetGraphicsPipelineState(*PipelineState);
		Immediate.BeginRenderPass(PassInfo, "OwnedPayloadPass");
		Immediate.PushConstants(
			EShaderStageFlags::Vertex, 4, static_cast<uint32>(PushBytes.size()), PushBytes.data());
		Immediate.SetShaderParameters(Shader.GetReference(), Parameters);
		Immediate.EndRenderPass();

		PassInfo.ColorRenderTargets[0] = nullptr;
		PassInfo.ColorClearValues[0] = FClearValueBinding(9, 9, 9, 9);
		std::fill(PushBytes.begin(), PushBytes.end(), 9);
		Parameters[0].Resource = nullptr;
		Parameters[0].BindingIndex = 99;
		EXPECT_GT(Texture->GetRefCount(), 1u);
		EXPECT_GT(Buffer->GetRefCount(), 1u);
		EXPECT_GT(Shader->GetRefCount(), 1u);
		EXPECT_GT(PipelineState->GetRefCount(), 1u);

		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.ObservedColorTarget, Texture.GetReference());
		EXPECT_EQ(Context.ObservedClearValue, (std::array<float, 4>{1, 2, 3, 4}));
		EXPECT_EQ(Context.ObservedPushConstants, (std::vector<uint8>{1, 2, 3, 4}));
		ASSERT_EQ(Context.ObservedShaderParameters.size(), 2u);
		EXPECT_EQ(Context.ObservedShader, Shader.GetReference());
		ASSERT_EQ(Context.ObservedShaderParameters[0].Resource->GetResourceType(), ERHIResourceType::BufferView);
		EXPECT_EQ(static_cast<FRHIBufferView*>(Context.ObservedShaderParameters[0].Resource)->GetBuffer(), Buffer.GetReference());
		EXPECT_EQ(Context.ObservedShaderParameters[0].BindingIndex, 2u);
		EXPECT_EQ(Context.ObservedShaderParameters[0].ArrayElement, 0u);
		EXPECT_EQ(Context.ObservedShaderParameters[1].ArrayElement, 1u);
		RHIFlushDeferredResources();
		EXPECT_EQ(Texture->GetRefCount(), 1u);
		EXPECT_EQ(Buffer->GetRefCount(), 1u);
		EXPECT_EQ(Shader->GetRefCount(), 1u);
		EXPECT_EQ(PipelineState->GetRefCount(), 1u);
	}

	TEST(FRHICommandListTests, ValidatesGraphicsPipelineAndRenderPassBalance)
	{
#if DO_CHECK
		FRHIRenderPassInfo PassInfo;
		EXPECT_DEATH(FRHICommandList().BeginRenderPass(PassInfo, "NoPipeline"), "");

		FRHICommandList Unbalanced;
		Unbalanced.SwitchPipeline(ERHIPipeline::Graphics);
		Unbalanced.BeginRenderPass(PassInfo, "Unbalanced");
		EXPECT_DEATH(Unbalanced.FinishRecording(), "");
#else
		GTEST_SKIP() << "Ordinary check contracts are disabled in Shipping.";
#endif
	}

	TEST(FRHICommandListTests, TransitionBatchesOwnDescriptorsAndResourcesUntilReplay)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(64);
		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"TransitionTexture", 16, 16, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource);
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>(TextureDesc);
		std::array BufferTransitions{
			FRHIBufferTransition{Buffer.GetReference(), 8, 24,
				ERHIAccess::Discard, ERHIAccess::VertexBufferRead}};
		std::array TextureTransitions{
			FRHITextureTransition{Texture.GetReference(),
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead}};
		const auto ExpectedBufferTransitions = BufferTransitions;
		const auto ExpectedTextureTransitions = TextureTransitions;

		FRHICommandList Commands;
		Commands.TransitionBuffers(BufferTransitions);
		Commands.TransitionTextures(TextureTransitions);
		Commands.FinishRecording();
		BufferTransitions[0].Offset = 40;
		TextureTransitions[0].RequiredAfter = ERHIAccess::TransferWrite;
		EXPECT_EQ(Buffer->GetRefCount(), 2u);
		EXPECT_EQ(Texture->GetRefCount(), 2u);

		Executor.Submit({&Commands}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.Operations,
			(std::vector<std::string>{"TransitionBuffers", "TransitionTextures"}));
		EXPECT_EQ(Context.ObservedBufferTransitions,
			(std::vector<FRHIBufferTransition>(ExpectedBufferTransitions.begin(), ExpectedBufferTransitions.end())));
		EXPECT_EQ(Context.ObservedTextureTransitions,
			(std::vector<FRHITextureTransition>(ExpectedTextureTransitions.begin(), ExpectedTextureTransitions.end())));
		EXPECT_GE(Executor.GetStats().RecordedPayloadBytes,
			sizeof(FRHIBufferTransition) + sizeof(FRHITextureTransition)
				+ sizeof(TRefCountPtr<FRHIBuffer>) + sizeof(TRefCountPtr<FRHITexture>));
		EXPECT_EQ(Buffer->GetRefCount(), 1u);
		EXPECT_EQ(Texture->GetRefCount(), 1u);
	}

	TEST(FRHICommandListTests, DestroyingRecorderReleasesTransitionResources)
	{
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(32);
		{
			FRHICommandList Commands;
			const std::array Transitions{
				FRHIBufferTransition::Whole(Buffer.GetReference(),
					ERHIAccess::Discard, ERHIAccess::VertexBufferRead)};
			Commands.TransitionBuffers(Transitions);
			EXPECT_EQ(Buffer->GetRefCount(), 2u);
		}
		EXPECT_EQ(Buffer->GetRefCount(), 1u);
	}

	TEST(FRHICommandListTests, TransitionCommandsWorkWithoutAnActivePipeline)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(32);
		const std::array Transitions{
			FRHIBufferTransition::Whole(Buffer.GetReference(),
				ERHIAccess::Discard, ERHIAccess::VertexBufferRead)};

		Executor.GetImmediateCommandList().TransitionBuffers(Transitions);
		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.Operations,
			(std::vector<std::string>{"TransitionBuffers"}));
		ASSERT_EQ(Context.OperationThreadRoles.size(), 1u);
		EXPECT_FALSE(Context.OperationThreadRoles[0]);
	}

	TEST(FRHICommandListTests, ThreadedTransitionReplayMatchesInlineOrder)
	{
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(32);
		const std::array Transitions{
			FRHIBufferTransition::Whole(Buffer.GetReference(),
				ERHIAccess::Discard, ERHIAccess::VertexBufferRead)};

		FRecordingCommandContext InlineContext;
		FRHICommandListExecutor InlineExecutor(InlineContext);
		InlineExecutor.GetImmediateCommandList().TransitionBuffers(Transitions);
		InlineExecutor.GetImmediateCommandList().EnqueueLambda(
			[&InlineContext]() { InlineContext.Operations.emplace_back("AfterTransition"); });
		InlineExecutor.Submit({}, ERHISubmitFlags::None);

		FRecordingCommandContext ThreadedContext;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor ThreadedExecutor(ThreadedContext, RHIThread);
		ThreadedExecutor.GetImmediateCommandList().TransitionBuffers(Transitions);
		ThreadedExecutor.GetImmediateCommandList().EnqueueLambda(
			[&ThreadedContext]() { ThreadedContext.Operations.emplace_back("AfterTransition"); });
		ThreadedExecutor.Submit({}, ERHISubmitFlags::None);
		ThreadedExecutor.CreateFence().Wait();

		EXPECT_EQ(InlineContext.Operations, ThreadedContext.Operations);
		ASSERT_EQ(ThreadedContext.OperationThreadRoles.size(), 1u);
		EXPECT_TRUE(ThreadedContext.OperationThreadRoles[0]);
		ThreadedExecutor.SetInlineMode();
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests, CopyCommandsOwnBatchesAndReplayAllDirections)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FBufferRHIRef SourceBuffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"CopySource", 256, 4, EBufferUsageFlags::SourceCopy));
		FBufferRHIRef DestinationBuffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"CopyDestination", 256, 4, EBufferUsageFlags::DestinationCopy));
		FTextureRHIRef SourceTexture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D("TextureSource", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::SourceCopy));
		FTextureRHIRef DestinationTexture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D("TextureDestination", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::DestinationCopy));
		std::array BufferRegions{FRHIBufferCopyRegion{0, 32, 32}};
		std::array BufferTextureRegions{FRHIBufferTextureCopyRegion{
			.BufferOffset = 0,
			.TextureExtent = {4, 4, 1}}};
		std::array TextureRegions{FRHITextureCopyRegion{.Extent = {4, 4, 1}}};

		FRHICommandList Commands;
		Commands.CopyBuffer(SourceBuffer, DestinationBuffer, BufferRegions);
		Commands.CopyBufferToTexture(SourceBuffer, DestinationTexture, BufferTextureRegions);
		Commands.CopyTextureToBuffer(SourceTexture, DestinationBuffer, BufferTextureRegions);
		Commands.CopyTexture(SourceTexture, DestinationTexture, TextureRegions);
		Commands.FinishRecording();
		BufferRegions[0].Size = 1;
		BufferTextureRegions[0].TextureExtent.Width = 1;
		TextureRegions[0].Extent.Width = 1;
		EXPECT_GT(SourceBuffer->GetRefCount(), 1u);
		EXPECT_GT(DestinationTexture->GetRefCount(), 1u);

		Executor.Submit({&Commands}, ERHISubmitFlags::None);
		EXPECT_EQ(Context.Operations, (std::vector<std::string>{
			"CopyBuffer", "CopyBufferToTexture", "CopyTextureToBuffer", "CopyTexture"}));
		EXPECT_EQ(Context.ObservedBufferCopyRegions,
			(std::vector<FRHIBufferCopyRegion>{{0, 32, 32}}));
		ASSERT_EQ(Context.ObservedBufferTextureCopyRegions.size(), 1u);
		EXPECT_EQ(Context.ObservedBufferTextureCopyRegions[0].TextureExtent.Width, 4u);
		ASSERT_EQ(Context.ObservedTextureCopyRegions.size(), 1u);
		EXPECT_EQ(Context.ObservedTextureCopyRegions[0].Extent.Width, 4u);
		EXPECT_GE(Executor.GetStats().RecordedPayloadBytes,
			sizeof(FRHIBufferCopyRegion) + 2 * sizeof(FRHIBufferTextureCopyRegion)
				+ sizeof(FRHITextureCopyRegion));
	}

	TEST(FRHICommandListTests, ThreadedCopyReplayMatchesInlineOrder)
	{
		FBufferRHIRef Source = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Source", 64, 4, EBufferUsageFlags::SourceCopy));
		FBufferRHIRef Destination = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Destination", 64, 4, EBufferUsageFlags::DestinationCopy));
		const std::array Regions{FRHIBufferCopyRegion{0, 16, 16}};

		FRecordingCommandContext InlineContext;
		FRHICommandListExecutor InlineExecutor(InlineContext);
		InlineExecutor.GetImmediateCommandList().CopyBuffer(Source, Destination, Regions);
		InlineExecutor.Submit({}, ERHISubmitFlags::None);

		FRecordingCommandContext ThreadedContext;
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		FRHICommandListExecutor ThreadedExecutor(ThreadedContext, RHIThread);
		ThreadedExecutor.GetImmediateCommandList().CopyBuffer(Source, Destination, Regions);
		ThreadedExecutor.Submit({}, ERHISubmitFlags::None);
		ThreadedExecutor.CreateFence().Wait();
		EXPECT_EQ(InlineContext.Operations, ThreadedContext.Operations);
		ASSERT_EQ(ThreadedContext.OperationThreadRoles.size(), 1u);
		EXPECT_TRUE(ThreadedContext.OperationThreadRoles[0]);
		ThreadedExecutor.SetInlineMode();
		RHIThread.Stop();
	}

	TEST(FRHICommandListTests, RejectsMalformedAndInPassTransitionBatches)
	{
#if DO_CHECK
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(32);
		const std::array ValidTransitions{
			FRHIBufferTransition::Whole(Buffer.GetReference(),
				ERHIAccess::Discard, ERHIAccess::VertexBufferRead)};
		const std::array InvalidTransitions{
			FRHIBufferTransition{nullptr, 0, 16,
				ERHIAccess::Discard, ERHIAccess::VertexBufferRead}};
		EXPECT_DEATH(FRHICommandList().TransitionBuffers(InvalidTransitions), "");

		FRHICommandList InPass;
		InPass.SwitchPipeline(ERHIPipeline::Graphics);
		InPass.BeginRenderPass(FRHIRenderPassInfo{}, "TransitionOrdering");
		EXPECT_DEATH(InPass.TransitionBuffers(ValidTransitions), "");
		InPass.TransitionBuffers(std::span<const FRHIBufferTransition>{});
		InPass.EndRenderPass();
#else
		GTEST_SKIP() << "Ordinary check contracts are disabled in Shipping.";
#endif
	}

	TEST(FRHICommandListTests, RejectsInPassCopiesAndIgnoresEmptyBatches)
	{
		FBufferRHIRef Source = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Source", 32, 4, EBufferUsageFlags::SourceCopy));
		FBufferRHIRef Destination = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Destination", 32, 4, EBufferUsageFlags::DestinationCopy));
		FRHICommandList Empty;
		Empty.CopyBuffer(nullptr, nullptr, {});
		EXPECT_EQ(Empty.GetNumRecordedCommands(), 0u);
#if DO_CHECK
		FRHICommandList InPass;
		InPass.SwitchPipeline(ERHIPipeline::Graphics);
		InPass.BeginRenderPass(FRHIRenderPassInfo{}, "CopyOrdering");
		const std::array Regions{FRHIBufferCopyRegion{0, 16, 16}};
		EXPECT_DEATH(InPass.CopyBuffer(Source, Destination, Regions), "");
		InPass.CopyBuffer(nullptr, nullptr, {});
		InPass.EndRenderPass();
#endif
	}

	TEST(FRHICommandListTests, BufferUploadsOwnSourceBytesUntilReplay)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(16);
		std::array<uint8, 4> Source{1, 2, 3, 4};

		FRHICommandList Commands;
		Commands.WriteBuffer(Buffer.GetReference(), Source.data(), Source.size(), 5);
		Commands.FinishRecording();
		std::fill(Source.begin(), Source.end(), 9);
		Executor.Submit({&Commands}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.ObservedBuffer, Buffer.GetReference());
		EXPECT_EQ(Context.ObservedBufferOffset, 5u);
		EXPECT_EQ(Context.ObservedBufferData, (std::vector<uint8>{1, 2, 3, 4}));
	}

	TEST(FRHICommandListTests, TextureUploadsOwnPackedSourceBytesUntilReplay)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
			"UploadTexture", 2, 2, EPixelFormat::RGBA8_UNORM);
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>(Desc);
		std::array<uint8, 16> Source{
			1, 2, 3, 4, 5, 6, 7, 8,
			9, 10, 11, 12, 13, 14, 15, 16};
		FUpdateTextureRegion2D Region(0, 0, 0, 0, 2, 2);

		Executor.GetImmediateCommandList().UpdateTexture2D(
			Texture.GetReference(), 0, 0, Region, 8, Source.data());
		std::fill(Source.begin(), Source.end(), 0);
		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.ObservedTextureData, (std::vector<uint8>{
			1, 2, 3, 4, 5, 6, 7, 8,
			9, 10, 11, 12, 13, 14, 15, 16}));
	}

	TEST(FRHICommandListTests, BufferLocksTransferOwnedBytesAtUnlock)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		TRefCountPtr<FTestBuffer> Buffer = MakeRefCount<FTestBuffer>(8);
		auto* Locked = static_cast<uint8*>(Immediate.LockBuffer(
			Buffer.GetReference(), 2, 3, EResourceLockMode::WriteOnly));
		Locked[0] = 4;
		Locked[1] = 5;
		Locked[2] = 6;

		EXPECT_DEATH(Executor.Submit({}, ERHISubmitFlags::None), "");
		Immediate.UnlockBuffer(Buffer.GetReference());
		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.ObservedBufferOffset, 2u);
		EXPECT_EQ(Context.ObservedBufferData, (std::vector<uint8>{4, 5, 6}));
	}

	TEST(FRHICommandListTests, DynamicUniformAllocationDoesNotSplitRecordedWork)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		Immediate.EnqueueLambda([&Context]() {
			Context.Operations.emplace_back("RecordedBeforeSync");
		});
		std::array<uint8, 16> UniformData{};

		Immediate.AllocateDynamicUniformBuffer(
			UniformData.data(), static_cast<uint32>(UniformData.size()));

		EXPECT_EQ(Context.Operations, (std::vector<std::string>{
			"AllocateDynamicUniformBuffer"}));
		EXPECT_EQ(Executor.GetCompletedSerial(), 0u);
		Executor.Submit({}, ERHISubmitFlags::None);
		EXPECT_EQ(Context.Operations, (std::vector<std::string>{
			"AllocateDynamicUniformBuffer", "RecordedBeforeSync"}));
	}

	TEST(FRHICommandListTests, GraphicsPipelineValueStateIsCohesiveAndComparable)
	{
		FGraphicsPipelineStateInitializer First;
		FGraphicsPipelineStateInitializer Second;

		EXPECT_EQ(First.RasterizerState, Second.RasterizerState);
		EXPECT_EQ(First.MultisampleState, Second.MultisampleState);
		EXPECT_EQ(First.DepthStencilState, Second.DepthStencilState);
		EXPECT_EQ(First.ColorBlendStates, Second.ColorBlendStates);
		EXPECT_EQ(First.RasterizerState.PolygonMode, ERHIPolygonMode::Fill);
		EXPECT_EQ(First.RasterizerState.CullMode, ERHICullMode::Back);
		EXPECT_EQ(First.RasterizerState.FrontFace, ERHIFrontFace::Clockwise);
		EXPECT_FALSE(First.DepthStencilState.bEnableTest);
		EXPECT_FALSE(First.DepthStencilState.bEnableWrite);
		EXPECT_EQ(First.DepthStencilState.CompareOp, ERHIDepthCompareOp::Less);
		EXPECT_FALSE(First.ColorBlendStates[0].bEnable);
		EXPECT_EQ(First.ColorBlendStates[0].ColorWriteMask,
			ERHIColorWriteMask::All);

		Second.ColorBlendStates[0] = FRHIColorBlendState::StraightAlpha();
		EXPECT_NE(First.ColorBlendStates, Second.ColorBlendStates);
		EXPECT_TRUE(Second.ColorBlendStates[0].bEnable);
		EXPECT_EQ(Second.ColorBlendStates[0].SrcColorFactor,
			ERHIBlendFactor::SrcAlpha);
		EXPECT_EQ(Second.ColorBlendStates[0].DstColorFactor,
			ERHIBlendFactor::OneMinusSrcAlpha);
		EXPECT_EQ(Second.ColorBlendStates[0].SrcAlphaFactor,
			ERHIBlendFactor::One);
		EXPECT_EQ(Second.ColorBlendStates[0].DstAlphaFactor,
			ERHIBlendFactor::OneMinusSrcAlpha);
	}

	TEST(FRHICommandListTests, GraphicsPipelineInitializerRejectsUnsupportedState)
	{
		FGraphicsPipelineStateInitializer Initializer;
		FTestShader VertexShader(EShaderFrequency::Vertex, 1);
		FTestShader FragmentShader(EShaderFrequency::Fragment, 2);
		FTestVertexDeclaration VertexDeclaration;
		Initializer.BoundShaders.VertexShader = &VertexShader;
		Initializer.BoundShaders.FragmentShader = &FragmentShader;
		Initializer.VertexDeclaration = &VertexDeclaration;
		Initializer.RenderTargetLayout.NumColorRenderTargets = 1;
		auto& Attachment = Initializer.RenderTargetLayout
			.ColorAttachments[0].RenderTarget;
		Attachment.Format = EPixelFormat::RGBA8_UNORM;
		Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		Attachment.InitialLayout = ERHITextureLayout::Undefined;
		Attachment.InitialAccess = ERHIAccess::None;
		Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
		ASSERT_TRUE(Initializer.IsValid());

		Initializer.RasterizerState.CullMode = ERHICullMode::Count;
		EXPECT_FALSE(Initializer.IsValid());
		Initializer.RasterizerState.CullMode = ERHICullMode::None;
		Initializer.ColorBlendStates[0].SrcColorFactor = ERHIBlendFactor::Count;
		EXPECT_FALSE(Initializer.IsValid());
		Initializer.ColorBlendStates[0].SrcColorFactor = ERHIBlendFactor::One;
		Initializer.PrimitiveTopology =
			FGraphicsPipelineStateInitializer::EPrimitiveTopology::Count;
		EXPECT_FALSE(Initializer.IsValid());
	}

	TEST(FRHICommandListTests, GraphicsPipelineKeyCanonicalizesInactiveState)
	{
		FTestShader VertexShader(EShaderFrequency::Vertex, 11);
		FTestShader FragmentShader(EShaderFrequency::Fragment, 12);
		FTestVertexDeclaration VertexDeclaration;
		FGraphicsPipelineStateInitializer First;
		First.BoundShaders = {&VertexShader, &FragmentShader};
		First.VertexDeclaration = &VertexDeclaration;
		First.RenderTargetLayout.NumColorRenderTargets = 1;
		auto& Attachment = First.RenderTargetLayout.ColorAttachments[0].RenderTarget;
		Attachment.Format = EPixelFormat::RGBA8_UNORM;
		Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
		FGraphicsPipelineStateInitializer Second = First;
		Second.RasterizerState.DepthBiasConstantFactor = 9.0f;
		Second.ColorBlendStates[0].SrcColorFactor = ERHIBlendFactor::DstColor;
		Second.ColorBlendStates[1] = FRHIColorBlendState::StraightAlpha();
		Second.RenderTargetLayout.ColorAttachments[1].RenderTarget.Format =
			EPixelFormat::BGRA8_UNORM;

		FGraphicsPipelineStateKey FirstKey;
		FGraphicsPipelineStateKey SecondKey;
		std::string Error;
		ASSERT_TRUE(BuildGraphicsPipelineStateKey(First, nullptr, FirstKey, Error)) << Error;
		ASSERT_TRUE(BuildGraphicsPipelineStateKey(Second, nullptr, SecondKey, Error)) << Error;
		EXPECT_EQ(FirstKey, SecondKey);
		EXPECT_EQ(FGraphicsPipelineStateKeyHasher{}(FirstKey),
			FGraphicsPipelineStateKeyHasher{}(SecondKey));

		Second.ColorBlendStates[0].ColorWriteMask = ERHIColorWriteMask::Red;
		ASSERT_TRUE(BuildGraphicsPipelineStateKey(Second, nullptr, SecondKey, Error)) << Error;
		EXPECT_NE(FirstKey, SecondKey);
	}

	TEST(FRHICommandListTests, GraphicsPipelineKeyValidatesVertexStreamRates)
	{
		FTestShader VertexShader(EShaderFrequency::Vertex, 21);
		FTestShader FragmentShader(EShaderFrequency::Fragment, 22);
		FVertexDeclarationElementList Elements{};
		Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 24,
			FRHIVertexElementIdentity::EInputRate::Vertex);
		Elements[1] = FVertexElement(0, 12, EVertexElementType::Float3, 1, 24,
			FRHIVertexElementIdentity::EInputRate::Instance);
		FTestVertexDeclaration VertexDeclaration(std::move(Elements));
		FGraphicsPipelineStateInitializer Initializer;
		Initializer.BoundShaders = {&VertexShader, &FragmentShader};
		Initializer.VertexDeclaration = &VertexDeclaration;
		Initializer.RenderTargetLayout.NumColorRenderTargets = 1;
		auto& Attachment = Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget;
		Attachment.Format = EPixelFormat::RGBA8_UNORM;
		Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
		FGraphicsPipelineStateKey Key;
		std::string Error;
		EXPECT_FALSE(BuildGraphicsPipelineStateKey(Initializer, nullptr, Key, Error));
		EXPECT_EQ(Error,
			"Graphics pipeline vertex stream stride or input rate is inconsistent.");
	}

	TEST(FRHICommandListTests, ReflectedBindingArraysValidateUpdateAndCompleteness)
	{
		FPipelineLayoutDesc Layout;
		Layout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Fragment, 3, ERHIBindingType::Sampler, 2);
		std::array<FRHIShaderParameterResource, 2> Resources{
			FRHIShaderParameterResource{
				.Resource = reinterpret_cast<FRHIResource*>(uintptr_t{1}),
				.SetIndex = 0, .BindingIndex = 3, .ArrayElement = 0,
				.Type = ERHIBindingType::Sampler},
			FRHIShaderParameterResource{
				.Resource = reinterpret_cast<FRHIResource*>(uintptr_t{2}),
				.SetIndex = 0, .BindingIndex = 3, .ArrayElement = 1,
				.Type = ERHIBindingType::Sampler}};
		std::string Error;
		EXPECT_TRUE(ValidateShaderParameterUpdate(Layout,
			EShaderStageFlags::Fragment, Resources, Error)) << Error;
		EXPECT_TRUE(ValidateShaderBindingCompleteness(Layout, Resources, Error))
			<< Error;

		Resources[1].ArrayElement = 2;
		EXPECT_FALSE(ValidateShaderParameterUpdate(Layout,
			EShaderStageFlags::Fragment, Resources, Error));
		Resources[1].ArrayElement = 0;
		EXPECT_FALSE(ValidateShaderBindingCompleteness(Layout, Resources, Error));
		Resources[1].ArrayElement = 1;
		Resources[1].Type = ERHIBindingType::Texture;
		EXPECT_FALSE(ValidateShaderParameterUpdate(Layout,
			EShaderStageFlags::Fragment, Resources, Error));
	}
} // namespace Durin
