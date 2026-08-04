#include <gtest/gtest.h>

#include "RHICommandList.h"
#include "RHIContext.h"

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

		class FRecordingCommandContext final : public IRHICommandContext
		{
		public:
			auto RHIBeginFrame() -> void override { Operations.emplace_back("BeginFrame"); }
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
			auto RHIEndDrawingViewport(FRHIViewport*, bool, bool) -> void override
			{
				Operations.emplace_back("EndDrawingViewport");
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
				OutData = {7, 8, 9};
				return true;
			}
			auto RHIAllocateDynamicUniformBuffer(
				const void*, uint32) -> FRHIUniformBufferRange override
			{
				Operations.emplace_back("AllocateDynamicUniformBuffer");
				return {};
			}
			auto RHIAcquireBackBuffer(FRHITexture*) -> void override
			{
				Operations.emplace_back("AcquireBackBuffer");
			}
			auto RHIBlockUntilGPUIdle() -> void override
			{
				Operations.emplace_back("BlockUntilGPUIdle");
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
			auto RHIDrawIndexed(uint32 IndexCount, uint32, int32) -> void override
			{
				Operations.emplace_back("Draw" + std::to_string(IndexCount));
			}

			std::vector<std::string> Operations;
			FRHITexture* ObservedColorTarget = nullptr;
			std::array<float, 4> ObservedClearValue{};
			std::vector<uint8> ObservedPushConstants;
			FRHIShader* ObservedShader = nullptr;
			std::vector<FRHIShaderParameterResource> ObservedShaderParameters;
			FRHIBuffer* ObservedBuffer = nullptr;
			uint32 ObservedBufferOffset = 0;
			std::vector<uint8> ObservedBufferData;
			std::vector<uint8> ObservedTextureData;
			bool* ResourceDestroyed = nullptr;
			bool ObservedResourceAliveAtSubmit = false;
			bool ObservedResourceAliveAtEndFrame = false;
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
		Executor.GetImmediateCommandList().EnqueueLambda(
			[Owned = Source, &Replayed]() { Replayed = Owned; });
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
			});
		Resource = nullptr;

		EXPECT_FALSE(bDestroyed);
		Executor.Submit({}, ERHISubmitFlags::DeleteResources);

		EXPECT_TRUE(bObservedDuringReplay);
		EXPECT_TRUE(bDestroyed);
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
		FRHICommandList CommandList;
		CommandList.FinishRecording();

		EXPECT_DEATH(CommandList.EnqueueLambda([]() {}), "");
	}

	TEST(FRHICommandListTests, DestroysOwnedCommandsOnceAfterReplay)
	{
		FRHICommandListExecutor Executor;
		int DestructionCount = 0;
		Executor.GetImmediateCommandList().EnqueueLambda(
			[Owned = std::unique_ptr<int, FCountedDelete>(
				new int(1), FCountedDelete{&DestructionCount})]() {});

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
					new int(1), FCountedDelete{&DestructionCount})]() {});
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
			});
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

	TEST(FRHICommandListTests, EmptySubmitDoesNotCreateASerial)
	{
		FRHICommandListExecutor Executor;

		EXPECT_EQ(Executor.Submit({}, ERHISubmitFlags::None), 0u);
		EXPECT_EQ(Executor.GetCompletedSerial(), 0u);
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

	TEST(FRHICommandListTests, GraphicsPayloadsAreOwnedUntilReplay)
	{
		FRecordingCommandContext Context;
		FRHICommandListExecutor Executor(Context);
		FRHICommandListImmediate& Immediate = Executor.GetImmediateCommandList();
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>();
		TRefCountPtr<FRHIBuffer> Buffer = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::CreateVertex("RecordedBuffer", 64));
		TRefCountPtr<FRHIShader> Shader = MakeRefCount<FRHIShader>(
			FRHIShaderDesc(EShaderFrequency::Vertex, FXxHash128{}));

		FRHIRenderPassInfo PassInfo;
		PassInfo.ColorRenderTargets[0] = Texture.GetReference();
		PassInfo.ColorClearValues[0] = FClearValueBinding(1, 2, 3, 4);
		std::vector<uint8> PushBytes{1, 2, 3, 4};
		std::vector<FRHIShaderParameterResource> Parameters{{
			Buffer.GetReference(), 1, 2, ERHIBindingType::UniformBuffer, 16, 32}};

		Immediate.SwitchPipeline(ERHIPipeline::Graphics);
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

		Executor.Submit({}, ERHISubmitFlags::None);

		EXPECT_EQ(Context.ObservedColorTarget, Texture.GetReference());
		EXPECT_EQ(Context.ObservedClearValue, (std::array<float, 4>{1, 2, 3, 4}));
		EXPECT_EQ(Context.ObservedPushConstants, (std::vector<uint8>{1, 2, 3, 4}));
		ASSERT_EQ(Context.ObservedShaderParameters.size(), 1u);
		EXPECT_EQ(Context.ObservedShader, Shader.GetReference());
		EXPECT_EQ(Context.ObservedShaderParameters[0].Resource, Buffer.GetReference());
		EXPECT_EQ(Context.ObservedShaderParameters[0].BindingIndex, 2u);
		EXPECT_EQ(Texture->GetRefCount(), 1u);
		EXPECT_EQ(Buffer->GetRefCount(), 1u);
		EXPECT_EQ(Shader->GetRefCount(), 1u);
	}

	TEST(FRHICommandListTests, ValidatesGraphicsPipelineAndRenderPassBalance)
	{
		FRHIRenderPassInfo PassInfo;
		EXPECT_DEATH(FRHICommandList().BeginRenderPass(PassInfo, "NoPipeline"), "");

		FRHICommandList Unbalanced;
		Unbalanced.SwitchPipeline(ERHIPipeline::Graphics);
		Unbalanced.BeginRenderPass(PassInfo, "Unbalanced");
		EXPECT_DEATH(Unbalanced.FinishRecording(), "");
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
} // namespace Durin
