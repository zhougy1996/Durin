#include "NativeRHIExecutionTestSupport.h"
#include "VulkanIntegrationTestFixture.h"
#include "VulkanTestSupport.h"
#include "VulkanRHITestEnvironment.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDiagnostics.h"
#include "VulkanGPUTiming.h"
#include "VulkanContext.h"
#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	class FVulkanDiagnosticsIntegrationTests : public FVulkanIntegrationTest
	{
	};

	namespace
	{
		struct FDebugMessageCapture
		{
			EVulkanDebugMessageSeverity Severity =
				EVulkanDebugMessageSeverity::Information;
			bool bGeneral = false;
			bool bValidation = false;
			bool bPerformance = false;
			bool bTruncated = false;
			std::string Message;
		};

		auto CaptureDebugMessage(
			const FVulkanClassifiedDebugMessage& Message, void* UserData) -> void
		{
			auto& Capture = *static_cast<FDebugMessageCapture*>(UserData);
			Capture = {
				.Severity = Message.Severity,
				.bGeneral = Message.bGeneral,
				.bValidation = Message.bValidation,
				.bPerformance = Message.bPerformance,
				.bTruncated = Message.bTruncated,
				.Message = std::string(Message.Message),
			};
		}

		struct FRecursiveDebugMessageContext
		{
			FVulkanDebugCallbackState* State = nullptr;
			uint32 SinkCallCount = 0;
		};

		auto ReenterDebugMessage(
			const FVulkanClassifiedDebugMessage&, void* UserData) -> void
		{
			auto& Context = *static_cast<FRecursiveDebugMessageContext*>(UserData);
			++Context.SinkCallCount;
			Context.State->HandleMessage(
				vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
				vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral,
				"recursive", &ReenterDebugMessage, &Context);
		}
	}

	TEST(FVulkanDebugUtilsTests, UnavailableNamingIsCountedAndNonFatal)
	{
		FVulkanDebugUtils DebugUtils;
		DebugUtils.NameObject(
			vk::Buffer(reinterpret_cast<VkBuffer>(uintptr_t{1})), "UnavailableBuffer");
		const FVulkanDebugUtilsStatistics Statistics = DebugUtils.Snapshot();
		EXPECT_EQ(Statistics.NamingAttemptCount, 1u);
		EXPECT_EQ(Statistics.NamingUnavailableSkipCount, 1u);
		EXPECT_EQ(Statistics.NamingFailureCount, 0u);
	}

	TEST(FVulkanDebugUtilsTests, UnavailableNamingDoesNotEvaluateNameFactories)
	{
		FVulkanDebugUtils DebugUtils;
		bool bEvaluated = false;
		auto MakeName = [&]() -> std::string {
			bEvaluated = true;
			throw std::logic_error("Unavailable diagnostics must not generate names.");
		};
		DebugUtils.NameObjectLazy(vk::Buffer{}, MakeName);
		EXPECT_EQ(DebugUtils.Snapshot().NamingAttemptCount, 0u);
		EXPECT_NO_THROW(DebugUtils.NameObjectLazy(
			vk::Buffer(reinterpret_cast<VkBuffer>(uintptr_t{1})), MakeName));
		EXPECT_FALSE(bEvaluated);
		const auto Statistics = DebugUtils.Snapshot();
		EXPECT_EQ(Statistics.NamingAttemptCount, 1u);
		EXPECT_EQ(Statistics.NamingUnavailableSkipCount, 1u);
		EXPECT_EQ(Statistics.NamingFailureCount, 0u);
	}

	TEST(FVulkanDebugUtilsTests, UnavailableNameLabelsAreCountedAndNonFatal)
	{
		FVulkanDebugUtils DebugUtils;
		const auto CommandBuffer = vk::CommandBuffer(
			reinterpret_cast<VkCommandBuffer>(uintptr_t{1}));
		EXPECT_FALSE(DebugUtils.BeginNameLabel(CommandBuffer, FName{}));
		EXPECT_EQ(DebugUtils.Snapshot().LabelUnavailableSkipCount, 0u);
		EXPECT_FALSE(DebugUtils.BeginNameLabel(CommandBuffer, FName("Pass_42")));
		EXPECT_EQ(DebugUtils.Snapshot().LabelUnavailableSkipCount, 1u);
		EXPECT_EQ(DebugUtils.Snapshot().LabelBeginCount, 0u);
	}

	TEST(FVulkanGPUTimingTests, ConvertsMaskedWrappedAndFractionalDurations)
	{
		bool bOverflow = false;
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			100, 125, 32, 1.0, bOverflow), 25u);
		EXPECT_FALSE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			0xfffffff0u, 0x10u, 32, 1.0, bOverflow), 32u);
		EXPECT_FALSE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			3, 8, 36, 52.083333, bOverflow), 260u);
		EXPECT_FALSE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			0, std::numeric_limits<uint64>::max(), 64, 2.0, bOverflow),
			std::numeric_limits<uint64>::max());
		EXPECT_TRUE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(0, 1, 0, 1.0, bOverflow), 0u);
		EXPECT_EQ(ConvertVulkanTimestampDuration(0, 1, 64, 0.0, bOverflow), 0u);
	}

	TEST(FVulkanDebugCallbackTests,
		ClassifiesBoundsCountsAndRejectsRecursiveDelivery)
	{
		FVulkanDebugCallbackState State;
		FDebugMessageCapture Capture;
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
				| vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
			"validation warning", &CaptureDebugMessage, &Capture);
		EXPECT_EQ(Capture.Severity, EVulkanDebugMessageSeverity::Warning);
		EXPECT_FALSE(Capture.bGeneral);
		EXPECT_TRUE(Capture.bValidation);
		EXPECT_TRUE(Capture.bPerformance);
		EXPECT_EQ(Capture.Message, "validation warning");

		std::string Oversized(
			FVulkanClassifiedDebugMessage::MaximumMessageBytes + 1, 'x');
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
			{}, Oversized.c_str(), &CaptureDebugMessage, &Capture);
		EXPECT_EQ(Capture.Severity, EVulkanDebugMessageSeverity::Error);
		EXPECT_TRUE(Capture.bGeneral);
		EXPECT_TRUE(Capture.bTruncated);
		EXPECT_EQ(Capture.Message.size(),
			FVulkanClassifiedDebugMessage::MaximumMessageBytes);

		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral, "verbose");
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral, "info");
		FRecursiveDebugMessageContext Recursive{.State = &State};
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral,
			"outer", &ReenterDebugMessage, &Recursive);

		const FVulkanDebugMessageStatistics Statistics = State.Snapshot();
		EXPECT_EQ(Statistics.TotalCount, 5u);
		EXPECT_EQ(Statistics.VerboseCount, 1u);
		EXPECT_EQ(Statistics.InformationCount, 2u);
		EXPECT_EQ(Statistics.WarningCount, 1u);
		EXPECT_EQ(Statistics.ErrorCount, 1u);
		EXPECT_EQ(Statistics.GeneralCount, 4u);
		EXPECT_EQ(Statistics.ValidationCount, 1u);
		EXPECT_EQ(Statistics.PerformanceCount, 1u);
		EXPECT_EQ(Statistics.TruncatedCount, 1u);
		EXPECT_EQ(Statistics.RecursionDropCount, 1u);
		EXPECT_EQ(Recursive.SinkCallCount, 1u);

		State.Reset();
		EXPECT_EQ(State.Snapshot().TotalCount, 0u);
	}

	TEST_F(FVulkanDiagnosticsIntegrationTests, OptionalDiagnosticsFollowValidationPolicy)
	{
		for (const char* Policy : {"off", "on"})
		{
			SCOPED_TRACE(Policy);
			_putenv_s("DURIN_VULKAN_VALIDATION", Policy);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
				const auto& Availability = static_cast<FVulkanDynamicRHI*>(GDynamicRHI)->GetDiagnosticAvailability();
				const bool Requested = std::string_view(Policy) == "on";
				EXPECT_EQ(Availability.bRequested, Requested);
				EXPECT_EQ(Availability.bDebugUtilsActive, Requested && Availability.bDebugUtilsSupported);
				EXPECT_EQ(Availability.bMessengerActive, Availability.bDebugUtilsActive);
			});
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanDiagnosticsIntegrationTests,
		PublicBufferNamingAndNestedDiagnosticRegionsRemainBalanced)
	{
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
			static_cast<FVulkanDynamicRHI*>(GDynamicRHI)->GetDebugUtils().ResetStatistics();

			FRHICommandListImmediate& Immediate = FRHICommandListImmediate::Get();
			FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(
				Immediate, FRHIBufferCreateDesc::Create(
					"Stage2NamedBuffer", 256, 16,
					EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static));
			ASSERT_TRUE(Buffer);
			std::string Utf8BoundaryName(254, 'a');
			Utf8BoundaryName += "\xc3\xa9";
			auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
			VulkanRHI->GetDebugUtils().NameObject(
				static_cast<FVulkanBuffer*>(Buffer.GetReference())->GetHandle(),
				Utf8BoundaryName);
			std::string Outer = "Stage2Outer";
			Immediate.BeginDiagnosticRegion(Outer);
			Outer = "MutatedAfterRecording";
			Immediate.BeginDiagnosticRegion("Stage2Inner");
			Immediate.EndDiagnosticRegion();
			Immediate.EndDiagnosticRegion();
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [] {
				auto* Context = GDynamicRHI->RHIGetDefaultContext();
				Context->RHIBeginDiagnosticRegion("NativeOuter");
				Context->RHIBeginDiagnosticRegion("NativeInner");
				Context->RHISubmitCommands();
				Context->RHIEndDiagnosticRegion();
				Context->RHIEndDiagnosticRegion();
				Context->RHISubmitCommands();
			});

			const auto BeforePass = VulkanRHI->GetDebugUtils().Snapshot();
			EXPECT_GE(BeforePass.NamingAttemptCount, 2u);
			EXPECT_EQ(BeforePass.NamingFailureCount, 0u);
			EXPECT_EQ(BeforePass.LabelBeginCount, 6u);
			EXPECT_EQ(BeforePass.LabelEndCount, 6u);
			EXPECT_EQ(BeforePass.ActiveLabelDepth, 0u);
			EXPECT_GE(BeforePass.LabelHighWater, 2u);

			FRHITextureCreateDesc RenderTargetDesc = FRHITextureCreateDesc::Create2D(
				"Stage2RegionRenderTarget", 4, 4, EPixelFormat::RGBA8_UNORM);
			RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable;
			FTextureRHIRef RenderTarget =
				GDynamicRHI->RHICreateTexture(Immediate, RenderTargetDesc);
			ASSERT_TRUE(RenderTarget);
			FRHIRenderPassInfo PassInfo;
			PassInfo.RenderTargetLayout.NumColorRenderTargets = 1;
			auto& Attachment =
				PassInfo.RenderTargetLayout.ColorAttachments[0].RenderTarget;
			Attachment.Format = EPixelFormat::RGBA8_UNORM;
			Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
			Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
			Attachment.InitialLayout = ERHITextureLayout::Undefined;
			Attachment.InitialAccess = ERHIAccess::None;
			Attachment.FinalLayout = ERHITextureLayout::ColorAttachment;
			Attachment.FinalAccess = ERHIAccess::ColorAttachmentReadWrite;
			PassInfo.ColorRenderTargets[0] = RenderTarget;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				auto* Context = GDynamicRHI->RHIGetDefaultContext();
				Context->RHIBeginDiagnosticRegion("Stage2OutsidePass");
				Context->RHIBeginRenderPass(PassInfo, "Stage2RenderPass_42");
				Context->RHIEndRenderPass();
				Context->RHIEndDiagnosticRegion();
			});
			const auto AfterPass = VulkanRHI->GetDebugUtils().Snapshot();
			EXPECT_EQ(AfterPass.LabelBeginCount - BeforePass.LabelBeginCount, 2u);
			EXPECT_EQ(AfterPass.LabelEndCount - BeforePass.LabelEndCount, 2u);
			EXPECT_EQ(AfterPass.ActiveLabelDepth, 0u);

			Buffer = nullptr;
			RenderTarget = nullptr;
			Immediate.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
		});
	}

	TEST_F(FVulkanDiagnosticsIntegrationTests,
		PublicTransferTimingBecomesReadyWithoutImplicitWait)
	{
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
				FRHICommandListImmediate& Immediate = FRHICommandListImmediate::Get();
				FGPUTimingQueryRHIRef Query =
					GDynamicRHI->RHICreateGPUTimingQuery();
				ASSERT_TRUE(Query);
				EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Query).State,
					ERHIGPUTimingResultState::Invalid);

				const auto Desc = FRHIBufferCreateDesc::Create(
					"Stage3TimingBuffer", 4096, 4,
					EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy);
				FBufferRHIRef Source = GDynamicRHI->RHICreateBuffer(Immediate, Desc);
				FBufferRHIRef Destination = GDynamicRHI->RHICreateBuffer(Immediate, Desc);
				ASSERT_TRUE(Source && Destination);
				Immediate.BeginGPUTimingQuery(Query);
				Immediate.TransitionBuffers(std::array{
					FRHIBufferTransition{Source, 0, 4096,
						ERHIAccess::Discard, ERHIAccess::TransferRead},
					FRHIBufferTransition{Destination, 0, 4096,
						ERHIAccess::Discard, ERHIAccess::TransferWrite}});
				Immediate.CopyBuffer(Source, Destination,
					std::array{FRHIBufferCopyRegion{0, 0, 4096}});
				Immediate.EndGPUTimingQuery(Query);
				Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
					ERHISubmitFlags::SubmitToGPU);
				EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Query).State,
					ERHIGPUTimingResultState::Pending);

				FRHIGPUTimingResult Result;
				for (uint32 Attempt = 0; Attempt < 500; ++Attempt)
				{
					GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
						auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
						PollVulkanGPUTimingForTest(*VulkanRHI);
					});
					Result = GDynamicRHI->RHIGetGPUTimingResult(Query);
					if (Result.State == ERHIGPUTimingResultState::Ready) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				EXPECT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
				EXPECT_GT(Result.DurationNanoseconds, 0u);
				FRHITextureCreateDesc RenderTargetDesc =
					FRHITextureCreateDesc::Create2D(
						"Stage3TimingRenderTarget", 16, 16, EPixelFormat::RGBA8_UNORM);
				RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable;
				FTextureRHIRef RenderTarget =
					GDynamicRHI->RHICreateTexture(Immediate, RenderTargetDesc);
				ASSERT_TRUE(RenderTarget);
				FRHIRenderPassInfo PassInfo;
				PassInfo.RenderTargetLayout.NumColorRenderTargets = 1;
				auto& Attachment =
					PassInfo.RenderTargetLayout.ColorAttachments[0].RenderTarget;
				Attachment.Format = EPixelFormat::RGBA8_UNORM;
				Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
				Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
				Attachment.InitialLayout = ERHITextureLayout::Undefined;
				Attachment.InitialAccess = ERHIAccess::None;
				Attachment.FinalLayout = ERHITextureLayout::ColorAttachment;
				Attachment.FinalAccess = ERHIAccess::ColorAttachmentReadWrite;
				PassInfo.ColorRenderTargets[0] = RenderTarget;
				Immediate.SwitchPipeline(ERHIPipeline::Graphics);
				Immediate.BeginGPUTimingQuery(Query);
				Immediate.BeginRenderPass(PassInfo, "Stage3TimedGraphicsPass");
				Immediate.EndRenderPass();
				Immediate.EndGPUTimingQuery(Query);
				Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
					ERHISubmitFlags::SubmitToGPU);
				EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Query).State,
					ERHIGPUTimingResultState::Pending);
				for (uint32 Attempt = 0; Attempt < 500; ++Attempt)
				{
					GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
						PollVulkanGPUTimingForTest(
							*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
					});
					Result = GDynamicRHI->RHIGetGPUTimingResult(Query);
					if (Result.State == ERHIGPUTimingResultState::Ready) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				EXPECT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
				EXPECT_GT(Result.DurationNanoseconds, 0u);

				Query = nullptr;
				Source = nullptr;
				Destination = nullptr;
				RenderTarget = nullptr;
				Immediate.ImmediateFlush(
					EImmediateFlushType::FlushRHIThreadFlushResources);

				FGPUTimingQueryRHIRef Orphaned =
					GDynamicRHI->RHICreateGPUTimingQuery();
				ASSERT_TRUE(Orphaned);
				Immediate.BeginGPUTimingQuery(Orphaned);
				Immediate.EndGPUTimingQuery(Orphaned);
				// Replay without submitting, then drop the caller's last reference.
				// The Vulkan context must own the recorded interval until submission.
				Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				Orphaned = nullptr;
				Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
				Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
					ERHISubmitFlags::SubmitToGPU);
				for (uint32 Attempt = 0; Attempt < 500; ++Attempt)
				{
					GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
						PollVulkanGPUTimingForTest(
							*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
					});
					if (GetVulkanGPUTimingStatisticsForTest(
						*static_cast<FVulkanDynamicRHI*>(GDynamicRHI))
						.PendingIntervals == 0) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Immediate.ImmediateFlush(
					EImmediateFlushType::FlushRHIThreadFlushResources);
				EXPECT_EQ(GetVulkanGPUTimingStatisticsForTest(
					*static_cast<FVulkanDynamicRHI*>(GDynamicRHI)).LiveIntervals, 0u);
			});
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanDiagnosticsIntegrationTests,
		TimingQueryPoolExhaustionAndReuseAreRecoverable)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "threaded");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
			auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
			std::vector<FGPUTimingQueryRHIRef> Queries;
			Queries.reserve(1280);
			for (uint32 Index = 0; Index < 1280; ++Index)
			{
				FGPUTimingQueryRHIRef Query =
					GDynamicRHI->RHICreateGPUTimingQuery();
				ASSERT_TRUE(Query) << Index;
				Queries.push_back(std::move(Query));
			}
			EXPECT_FALSE(GDynamicRHI->RHICreateGPUTimingQuery());
			auto Statistics = GetVulkanGPUTimingStatisticsForTest(*VulkanRHI);
			EXPECT_EQ(Statistics.IntervalCapacity, 1280u);
			EXPECT_EQ(Statistics.AllocatedPages, 20u);
			EXPECT_EQ(Statistics.LiveIntervals, 1280u);
			EXPECT_EQ(Statistics.IntervalHighWater, 1280u);
			EXPECT_EQ(Statistics.ExhaustionCount, 1u);

			Queries.clear();
			FRHICommandListImmediate::Get().ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			Statistics = GetVulkanGPUTimingStatisticsForTest(*VulkanRHI);
			EXPECT_EQ(Statistics.LiveIntervals, 0u);
			FGPUTimingQueryRHIRef Reused =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Reused);
			EXPECT_GE(GetVulkanGPUTimingStatisticsForTest(*VulkanRHI).ReuseCount, 1u);
			Reused = nullptr;
			FRHICommandListImmediate::Get().ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
		});
	}

	TEST_F(FVulkanDiagnosticsIntegrationTests,
		DiagnosticSnapshotComposesAuthoritiesAndResetPreservesLiveState)
	{
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
			FGPUTimingQueryRHIRef Query = GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Query);
			FRHIDiagnosticSnapshot Before;
			FRHIDiagnosticSnapshot Repeated;
			FRHIDiagnosticSnapshot After;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				Before = GDynamicRHI->RHIGetDiagnosticSnapshot();
				Repeated = GDynamicRHI->RHIGetDiagnosticSnapshot();
				GDynamicRHI->RHIResetDiagnosticStatistics();
				After = GDynamicRHI->RHIGetDiagnosticSnapshot();
			});

			EXPECT_TRUE(Before.Availability.bRequested);
			EXPECT_TRUE(Before.Availability.bDebugUtilsActive);
			EXPECT_TRUE(Before.Availability.bValidationLayerActive);
			EXPECT_TRUE(Before.Availability.bMessengerActive);
			EXPECT_EQ(Before.Timing.IntervalCapacity, 1280u);
			EXPECT_EQ(Before.Timing.AllocatedPages, 1u);
			EXPECT_EQ(Before.Timing.LiveIntervals, 1u);
			EXPECT_EQ(Before.Executor.Mode, ERHICommandListExecutorMode::Threaded);
			EXPECT_GT(Before.Naming.NamingAttempts, 0u);
			EXPECT_EQ(Repeated.Executor.RecordedCommandCount,
				Before.Executor.RecordedCommandCount);
			EXPECT_EQ(Repeated.Executor.SynchronousOperationCount,
				Before.Executor.SynchronousOperationCount);
			EXPECT_EQ(Repeated.Completion.LastSubmittedToken,
				Before.Completion.LastSubmittedToken);
			EXPECT_EQ(Repeated.Messages.Total, Before.Messages.Total);
			EXPECT_EQ(Repeated.Naming.NamingAttempts, Before.Naming.NamingAttempts);
			EXPECT_EQ(Repeated.Timing.ResultPollCount,
				Before.Timing.ResultPollCount);

			EXPECT_EQ(After.Availability.bDebugUtilsActive,
				Before.Availability.bDebugUtilsActive);
			EXPECT_EQ(After.Executor.RecordedCommandCount,
				Before.Executor.RecordedCommandCount);
			EXPECT_EQ(After.PipelineCache.GraphicsPipelines.Capacity,
				Before.PipelineCache.GraphicsPipelines.Capacity);
			EXPECT_EQ(After.PipelineCache.GraphicsPipelines.Occupancy,
				Before.PipelineCache.GraphicsPipelines.Occupancy);
			EXPECT_EQ(After.Completion.LastSubmittedToken,
				Before.Completion.LastSubmittedToken);
			EXPECT_EQ(After.Completion.PendingSubmissions,
				Before.Completion.PendingSubmissions);
			EXPECT_EQ(After.Memory.RetirementPendingCount,
				Before.Memory.RetirementPendingCount);
			for (uint32 ClassIndex = 0;
				ClassIndex < FRHIMemoryStatistics::AllocationClassCount; ++ClassIndex)
			{
				EXPECT_EQ(After.Memory.Classes[ClassIndex].LiveAllocationCount,
					Before.Memory.Classes[ClassIndex].LiveAllocationCount);
				EXPECT_EQ(After.Memory.Classes[ClassIndex].LiveBytes,
					Before.Memory.Classes[ClassIndex].LiveBytes);
				EXPECT_EQ(After.Memory.Classes[ClassIndex].ArenaLiveBytes,
					Before.Memory.Classes[ClassIndex].ArenaLiveBytes);
			}
			EXPECT_EQ(After.Timing.AllocatedPages, 1u);
			EXPECT_EQ(After.Timing.LiveIntervals, 1u);
			EXPECT_EQ(After.Timing.IntervalHighWater, 1u);
			EXPECT_EQ(After.Timing.ExhaustionCount, 0u);
			EXPECT_EQ(After.Messages.Total, 0u);
			EXPECT_EQ(After.Naming.NamingAttempts, 0u);
			EXPECT_EQ(After.Naming.ActiveRegionDepth, 0u);

			Query = nullptr;
			FRHICommandListImmediate::Get().ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				After = GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(After.Timing.LiveIntervals, 0u);
		});
	}

	TEST_F(FVulkanDiagnosticsIntegrationTests,
		DiagnosticSnapshotPublishesModeIndependentCapabilities)
	{
		std::array<FRHIDiagnosticSnapshot, 2> Snapshots;
		const std::array Modes{"inline", "threaded"};
		_putenv_s("DURIN_VULKAN_VALIDATION", "off");
		for (size_t Index = 0; Index < Modes.size(); ++Index)
		{
			SCOPED_TRACE(Modes[Index]);
			_putenv_s("DURIN_RHI_EXECUTION", Modes[Index]);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
				FGPUTimingQueryRHIRef Query = GDynamicRHI->RHICreateGPUTimingQuery();
				ASSERT_TRUE(Query);
				GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
					Snapshots[Index] = GDynamicRHI->RHIGetDiagnosticSnapshot();
				});
				EXPECT_EQ(Snapshots[Index].Executor.Mode, Index == 0
					? ERHICommandListExecutorMode::Inline
					: ERHICommandListExecutorMode::Threaded);
				Query = nullptr;
				FRHICommandListImmediate::Get().ImmediateFlush(
					EImmediateFlushType::FlushRHIThreadFlushResources);
			});
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}

		EXPECT_EQ(Snapshots[0].Availability.bRequested,
			Snapshots[1].Availability.bRequested);
		EXPECT_EQ(Snapshots[0].Availability.bDebugUtilsActive,
			Snapshots[1].Availability.bDebugUtilsActive);
		EXPECT_EQ(Snapshots[0].Timing.IntervalCapacity,
			Snapshots[1].Timing.IntervalCapacity);
		EXPECT_EQ(Snapshots[0].Timing.AllocatedPages,
			Snapshots[1].Timing.AllocatedPages);
		EXPECT_EQ(Snapshots[0].Timing.LiveIntervals,
			Snapshots[1].Timing.LiveIntervals);
		EXPECT_EQ(Snapshots[0].PipelineCache.GraphicsPipelines.Capacity,
			Snapshots[1].PipelineCache.GraphicsPipelines.Capacity);
	}
}
