#include <gtest/gtest.h>
#include "Backend/RHICreation.h"

#include "RHIPipelineCreation.h"
#include "PipelineStateCache.h"
#include "Backend/RHIPipelineStateCacheBackend.h"
#include "PipelineCompileQueue.h"
#include "RHICapabilities.h"
#include "RHICommandList.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/Task.h"

namespace Durin
{
	class FRHIPipelineCreationTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
		}
		auto TearDown() -> void override
		{
			ShutdownTaskScheduler();
			RHIFlushDeferredResources();
		}
		static auto Capabilities() -> FRHICapabilities
		{
			FRHICapabilities Result;
			Result.MaxComputeWorkGroupCount = {65535, 65535, 65535};
			return Result;
		}
		static auto Backend(std::function<FComputePipelineStateRHIRef(
			const FRHIComputePipelineCreationInputs&, const FComputePipelineStateKey&)> Create)
			-> FRHIPipelineCompileBackend
		{
			return {
				.FindGraphics = [](const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
				.FindCompute = [](const auto&) -> FComputePipelineStateRHIRef { return {}; },
				.CreateGraphics = [](const auto&, const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
				.CreateCompute = std::move(Create),
			};
		}
	};

	TEST_F(FRHIPipelineCreationTests, SharedCompletionPublishesOneReadyOrFailedOutcome)
	{
		for (const auto Expected : {ERHIPipelineRequestState::Ready, ERHIPipelineRequestState::Failed})
		{
			SCOPED_TRACE(static_cast<int>(Expected));
			std::promise<void> Release;
			auto Released = Release.get_future().share();
			FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) -> FComputePipelineStateRHIRef {
				Released.wait();
				return Expected == ERHIPipelineRequestState::Ready ? MakeRefCount<FRHIComputePipelineState>() : nullptr;
			}));
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			auto Request = Owner.RequestCompute(Initializer, "shared outcome");
			auto Peer = Owner.RequestCompute(Initializer, "same compilation");
			EXPECT_EQ(Request, Peer);
			EXPECT_EQ(Request.GetTaskHandle().GetTaskId(), Peer.GetTaskHandle().GetTaskId());
			EXPECT_EQ(Request.GetStateObservation(), Peer.GetStateObservation());
			std::promise<FRHIPipelineCreationResult> Publication;
			auto Published = Publication.get_future();
			auto Subscription = Tasks::SubscribeTask(Peer.GetTaskHandle(), [&](ETaskState) {
				EXPECT_TRUE(Peer.GetTaskHandle().IsCompleted());
				EXPECT_EQ(Peer.GetState(), Expected);
				// Notification is outside the queue lock and may reenter acquisition.
				EXPECT_EQ(Owner.RequestCompute(Initializer, "completion lookup"), Peer);
				Publication.set_value(Peer.GetResult());
			});
			Release.set_value();
			const auto Ready = Published.wait_for(std::chrono::seconds(5));
			EXPECT_EQ(Ready, std::future_status::ready);
			if (Ready == std::future_status::ready)
			{
				const auto Result = Published.get();
				EXPECT_EQ(Result.State, Expected);
				EXPECT_EQ(Result.Compute != nullptr, Expected == ERHIPipelineRequestState::Ready);
			}
			Owner.StopAndWait();
			EXPECT_EQ(ETaskState::Succeeded, Request.GetTaskHandle().GetState());
		}
	}

	TEST_F(FRHIPipelineCreationTests, DroppedHandlesKeepSharedCompletionPendingUntilCompilationFinishes)
	{
		std::promise<void> Entered, Release;
		auto Released = Release.get_future().share();
		std::atomic<bool> Retired = false;
		FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			struct FRetire { std::atomic<bool>& Flag; ~FRetire() { Flag = true; } } Capture{Retired};
			Entered.set_value(); Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer; Initializer.ComputeShader = Shader;
		auto Request = Owner.RequestCompute(Initializer, "dropped observer");
		ASSERT_TRUE(Request.IsAccepted());
		Entered.get_future().wait();
		auto Signal = Request.GetTaskHandle();
		Request = {};
		EXPECT_FALSE(Signal.IsCompleted());
		auto Replacement = Owner.RequestCompute(Initializer, "reattach to shared compilation");
		EXPECT_EQ(Replacement.GetTaskHandle().GetTaskId(), Signal.GetTaskId());
		Replacement = {};
		EXPECT_FALSE(Retired.load());
		Release.set_value(); Owner.StopAndWait();
		EXPECT_TRUE(Signal.IsCompleted());
		EXPECT_TRUE(Retired.load());
	}

	TEST_F(FRHIPipelineCreationTests, IdleQueueRejectsReuseAfterSchedulerRestart)
	{
		FPipelineCompileQueue Owner(Capabilities(), Backend([](const auto&, const auto&) {
			ADD_FAILURE() << "A queue from the previous scheduler must not create device work.";
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		const auto Request = Owner.RequestCompute(Initializer, "stale queue");
		EXPECT_FALSE(Request.IsAccepted());
		EXPECT_EQ(Request.GetRejection(), ERHIPipelineRequestRejection::Unsupported);
	}
	TEST_F(FRHIPipelineCreationTests, PriorityBoostReordersQueuedSharedWorkWithoutPreemptingActiveCompile)
	{
		std::promise<void> Release, Started;
		auto Released = Release.get_future().share();
		auto Running = Started.get_future();
		std::vector<std::string> Order;
		FPipelineCompileQueue Queue(Capabilities(), Backend([&](const auto& Input, const auto&) {
			Order.push_back(Input.DebugName);
			if (Input.DebugName == "active") { Started.set_value(); Released.wait(); }
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		auto Request = [&](uint32 Size, std::string_view Name) {
			FComputePipelineStateInitializer Input; Input.ComputeShader = Shader;
			Input.PipelineLayout.PushConstantRanges.push_back({EShaderStageFlags::Compute, 0, Size});
			return Queue.RequestCompute(Input, Name);
		};
		auto Active = Request(4, "active");
		EXPECT_EQ(Running.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		auto Medium = Request(8, "medium");
		auto First = Request(12, "first urgent");
		auto Peer = Request(12, "shared observer");
		auto Second = Request(16, "second urgent");
		Peer.BoostPriority(EPSOPrecachePriority::Highest);
		Peer.BoostPriority(EPSOPrecachePriority::Low);
		Second.BoostPriority(EPSOPrecachePriority::Highest);
		EXPECT_EQ(First.GetPriority(), EPSOPrecachePriority::Highest);
		EXPECT_EQ(First.GetTaskHandle().GetTaskId(), Peer.GetTaskHandle().GetTaskId());
		Peer = {};
		Release.set_value();
		EXPECT_TRUE(Active.Wait()); EXPECT_TRUE(First.Wait()); EXPECT_TRUE(Second.Wait()); EXPECT_TRUE(Medium.Wait());
		Queue.StopAndWait();
		EXPECT_EQ(Order, (std::vector<std::string>{"active", "first urgent", "second urgent", "medium"}));
		EXPECT_EQ(Queue.GetStatistics().NativeRequests, 4u);
	}

	TEST_F(FRHIPipelineCreationTests, CacheSharesPendingCompilationAcrossPrecacheAndDraw)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.PushConstantRanges.push_back({EShaderStageFlags::Compute, 0, 4});
		std::promise<void> Release;
		auto Released = Release.get_future().share();
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			++Calls;
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto& Cache = Owner;
		auto Caps = Capabilities();
		auto Key = BuildComputePipelineStateKey(Initializer, &Caps);
		ASSERT_TRUE(Key);
		auto First = Cache.GetCompute(Initializer, "precache");
		auto Second = FRHIPipelineStateCacheBackend::GetComputeValidated(Cache, Initializer, "draw", *Key);
		EXPECT_TRUE(First);
		EXPECT_TRUE(Second);
		if (First && Second)
		{
			EXPECT_EQ(*First, *Second);
			EXPECT_EQ((*First)->GetState(), ERHIPipelineRequestState::Pending);
			EXPECT_EQ((*First)->GetPipelineLayout()->PushConstantRanges.size(), 1u);
			EXPECT_FALSE((*First)->GetRHIPipeline());
			EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 1u);
			First = std::unexpected(ERHIPipelineRequestRejection::Unsupported);
			EXPECT_EQ((*Second)->GetState(), ERHIPipelineRequestState::Pending);
		}
		Release.set_value();
		if (Second) EXPECT_TRUE((*Second)->Wait());
		EXPECT_EQ(Calls, 1u);
	}


	TEST_F(FRHIPipelineCreationTests, CacheSharesReadyIdentityAndDeviceRetirementInvalidatesSurvivingHandles)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) -> FComputePipelineStateRHIRef {
										 ++Calls;
										 return MakeRefCount<FRHIComputePipelineState>();
									 }));
		auto& Cache = Owner;
		auto Failed = Cache.GetCompute(Initializer, "failure");
		ASSERT_TRUE(Failed);
		EXPECT_TRUE((*Failed)->Wait());
		EXPECT_TRUE((*Failed)->GetResult().Compute);
		auto Retried = Cache.GetCompute(Initializer, "retry");
		ASSERT_TRUE(Retried);
		EXPECT_EQ(*Failed, *Retried);
		EXPECT_TRUE((*Retried)->Wait());
		EXPECT_TRUE((*Retried)->GetRHIPipeline());
		EXPECT_EQ(Calls, 1u);
		Owner.StopAndWait();
		Owner.StopAndWait();
		EXPECT_EQ((*Retried)->GetState(), ERHIPipelineRequestState::Ready);
		EXPECT_EQ(Cache.GetCompute(Initializer, "closed").error(), ERHIPipelineRequestRejection::Closed);
		Owner.ReleaseResources();
		Owner.ReleaseResources();
		EXPECT_EQ((*Retried)->GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_FALSE((*Retried)->GetRHIPipeline());
		EXPECT_TRUE((*Retried)->IsComplete());
	}

	TEST_F(FRHIPipelineCreationTests, CacheReclaimsExpiredKeysUnderMetadataPressureBeforeEntryCapacity)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.PushConstantRanges.push_back({EShaderStageFlags::Compute, 0, 4});
		std::atomic<uint64> CacheEntryBytes = 0;
		std::atomic<uint32> CacheLeases = 0;
		auto Callbacks = Backend([](const auto&, const auto&) { return MakeRefCount<FRHIComputePipelineState>(); });
		Callbacks.ReserveMetadata = [&](uint64 Bytes) -> std::shared_ptr<void> {
			uint64 Expected = 0;
			CacheEntryBytes.compare_exchange_strong(Expected, Bytes);
			if (Bytes != CacheEntryBytes.load()) return {};
			if (CacheLeases.fetch_add(1) != 0)
			{
				CacheLeases.fetch_sub(1);
				throw FRHIResourceCapacityError("pipeline metadata budget exhausted");
			}
			return {nullptr, [&](void*) { CacheLeases.fetch_sub(1); }};
		};
		FRHIPipelineStateCache Owner(Capabilities(), std::move(Callbacks));
		auto& Cache = Owner;
		{
			auto First = Cache.GetCompute(Initializer, "first");
			ASSERT_TRUE(First);
			EXPECT_TRUE((*First)->Wait());
		}
		EXPECT_EQ(CacheLeases, 1u);
		Initializer.PipelineLayout.PushConstantRanges[0].Offset = 4;
		auto Second = Cache.GetCompute(Initializer, "second");
		ASSERT_TRUE(Second);
		EXPECT_TRUE((*Second)->Wait());
		EXPECT_EQ(CacheLeases, 1u);
		Owner.ReleaseResources();
		Second = std::unexpected(ERHIPipelineRequestRejection::Closed);
		EXPECT_EQ(CacheLeases, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, ColdWorkerAdmissionStaysOnCallerAndDoesNotNestCompilation)
	{
		std::promise<void> Release, ParentFinished;
		auto Released = Release.get_future().share();
		auto Finished = ParentFinished.get_future();
		std::atomic<uint64> AdmissionTask = 0;
		auto Callbacks = Backend([&](const auto&, const auto&) {
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		});
		Callbacks.FindCompute = [&](const auto&) -> FComputePipelineStateRHIRef {
			AdmissionTask = Private::FTaskRuntimeAccess::GetCurrentTaskId();
			return {};
		};
		FRHIPipelineStateCache Owner(Capabilities(), std::move(Callbacks));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		auto Parent = Tasks::LaunchTask("cold worker admission", [&] {
			auto Result = Owner.GetCompute(Initializer, "cold PSO");
			if (Result)
			{
				EXPECT_EQ((*Result)->GetState(), ERHIPipelineRequestState::Pending);
				EXPECT_FALSE((*Result)->CanWait());
				EXPECT_FALSE((*Result)->Wait());
			}
			return Result;
		});
		auto Subscription = Parent.Subscribe([&](ETaskState) { ParentFinished.set_value(); });
		// Do not help/retract the parent: it must finish on the sole Worker while
		// compilation remains gated, without implicitly nesting its drain task.
		const auto Completion = Finished.wait_for(std::chrono::seconds(5));
		EXPECT_EQ(Completion, std::future_status::ready);
		EXPECT_EQ(AdmissionTask.load(), Parent.GetTaskId());
		Release.set_value();
		ASSERT_TRUE(Parent.Wait(5));
		const auto& Result = Parent.GetResult();
		ASSERT_TRUE(Result);
		EXPECT_TRUE((*Result)->Wait());
	}

	TEST_F(FRHIPipelineCreationTests, QueueAdmitsOrdinaryTasksButRejectsIndependentCpuWork)
	{
		FPipelineCompileQueue Owner(Capabilities(), Backend([](const auto&, const auto&) {
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		for (const auto Executor : {Tasks::ETaskExecutor::Worker, Tasks::ETaskExecutor::BlockingIO})
		{
			auto Task = Tasks::LaunchTask(Executor, {.DebugName = "direct queue admission"}, [&] {
				return Owner.RequestCompute(Initializer, "task PSO");
			});
			ASSERT_TRUE(Task.Wait(5));
			const auto& Request = Task.GetResult();
			ASSERT_TRUE(Request.IsAccepted());
			EXPECT_TRUE(Request.Wait());
		}
		auto Independent = Tasks::LaunchIndependentTask("direct queue CPU rejection", [&] {
			return Owner.RequestCompute(Initializer, "independent PSO").GetRejection();
		});
		EXPECT_EQ(Independent.GetResult(), ERHIPipelineRequestRejection::Unsupported);
	}

	TEST_F(FRHIPipelineCreationTests, ConcurrentWorkerAdmissionsShareCompilationAndCacheClose)
	{
		ShutdownTaskScheduler();
		ASSERT_TRUE(InitializeTaskScheduler(4));
		std::promise<void> Release;
		auto Released = Release.get_future().share();
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			++Calls;
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		using TResult = std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>;
		std::vector<Tasks::TTask<TResult>> Admissions;
		for (uint32 Index = 0; Index < 8; ++Index)
			Admissions.push_back(Tasks::LaunchTask("concurrent PSO admission", [&] { return Owner.GetCompute(Initializer, "shared PSO"); }));
		std::vector<FComputePipelineStateRef> Pipelines;
		for (const auto& Task : Admissions)
		{
			const bool Completed = Task.Wait(5);
			EXPECT_TRUE(Completed);
			if (Completed && Task.GetResult()) Pipelines.push_back(*Task.GetResult());
		}
		Release.set_value();
		ASSERT_EQ(Pipelines.size(), Admissions.size());
		for (const auto& Pipeline : Pipelines)
		{
			EXPECT_EQ(Pipeline, Pipelines.front());
			EXPECT_TRUE(Pipeline->Wait());
		}
		EXPECT_EQ(Calls.load(), 1u);
		auto ConcurrentStop = std::async(std::launch::async, [&] { Owner.StopAndWait(); });
		Owner.StopAndWait();
		ConcurrentStop.get();
		EXPECT_EQ(Owner.GetCompute(Initializer, "closed").error(), ERHIPipelineRequestRejection::Closed);
		EXPECT_EQ(Pipelines.front()->GetState(), ERHIPipelineRequestState::Ready);
		Owner.ReleaseResources();
		EXPECT_FALSE(Pipelines.front()->GetRHIPipeline());
	}

	TEST_F(FRHIPipelineCreationTests, CacheRejectsInvalidDescriptionsAndIndependentCpuTasks)
	{
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			++Calls;
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		FComputePipelineStateInitializer Initializer;
		EXPECT_EQ(Owner.GetCompute(Initializer, "invalid").error(), ERHIPipelineRequestRejection::InvalidDescription);
		FGraphicsPipelineStateInitializer GraphicsInitializer;
		EXPECT_EQ(Owner.GetGraphics(GraphicsInitializer, "invalid graphics").error(),
			ERHIPipelineRequestRejection::InvalidDescription);
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		Initializer.ComputeShader = Shader;
		auto Task = Tasks::LaunchIndependentTask("cache leaf rejection", [&] {
			return Owner.GetCompute(Initializer, "cpu leaf").error();
		});
		EXPECT_EQ(Task.GetResult(), ERHIPipelineRequestRejection::Unsupported);
		auto Parent = Tasks::LaunchIndependentTask("cache inherited CPU rejection", [&] {
			auto Child = Tasks::LaunchTask("cache inherited CPU child", [&] {
				return Owner.GetCompute(Initializer, "cpu child").error();
			});
			return Child.GetResult();
		});
		EXPECT_EQ(Parent.GetResult(), ERHIPipelineRequestRejection::Unsupported);
		EXPECT_EQ(Calls, 0u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, ValidatedCacheEntryPreservesAdmissionGates)
	{
		std::atomic<uint32> Calls = 0;
		auto Caps = Capabilities();
		FRHIPipelineStateCache Owner(Caps, Backend([&](const auto&, const auto&) {
			++Calls;
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		auto Key = BuildComputePipelineStateKey(Initializer, &Caps);
		ASSERT_TRUE(Key);
		EXPECT_EQ(FRHIPipelineStateCacheBackend::GetComputeValidated(Owner, Initializer,
			std::string(1024 * 1024, 'x'), *Key).error(), ERHIPipelineRequestRejection::CapacityExceeded);
		auto Task = Tasks::LaunchIndependentTask("validated cache leaf rejection", [&] {
			return FRHIPipelineStateCacheBackend::GetComputeValidated(Owner, Initializer, "cpu leaf", *Key).error();
		});
		EXPECT_EQ(Task.GetResult(), ERHIPipelineRequestRejection::Unsupported);
		Owner.StopAndWait();
		EXPECT_EQ(FRHIPipelineStateCacheBackend::GetComputeValidated(Owner, Initializer,
			"closed", *Key).error(), ERHIPipelineRequestRejection::Closed);
		EXPECT_EQ(Calls, 0u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, SharesStateAndOwnsInputsAfterCallerDropsItsHandle)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		const auto* ShaderAddress = Shader.GetReference();
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Compute, 3, ERHIBindingType::UniformBuffer);
		std::string Name = "owned name";
		std::promise<void> Entered, Release;
		auto EnteredFuture = Entered.get_future();
		auto Released = Release.get_future().share();
		std::atomic<uint32> Calls = 0;
		FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto& Inputs, const auto& Key) {
			++Calls;
			Entered.set_value();
			Released.wait();
			EXPECT_EQ(Inputs.DebugName, "owned name");
			EXPECT_EQ(Inputs.Initializer.ComputeShader, ShaderAddress);
			EXPECT_GT(ShaderAddress->GetRefCount(), 0u);
			EXPECT_EQ(Inputs.Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot, 3u);
			EXPECT_EQ(Key.PipelineLayout->BindingLayouts[0].BindingLayouts[0].Slot, 3u);
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto First = Owner.RequestCompute(Initializer, Name);
		auto Second = Owner.RequestCompute(Initializer, "different diagnostic name");
		EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		Initializer.PipelineLayout.BindingLayouts.clear();
		Initializer.ComputeShader = nullptr;
		Shader = nullptr;
		Name.assign("modified");
		EXPECT_EQ(First, Second);
		EXPECT_EQ(First.GetTaskHandle().GetTaskId(), Second.GetTaskHandle().GetTaskId());
		First = {};
		EXPECT_EQ(Second.GetState(), ERHIPipelineRequestState::Pending);
		Release.set_value();
		EXPECT_TRUE(Second.Wait());
		EXPECT_TRUE(Second.GetResult().Compute);
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Owner.GetStatistics().SharedPendingHits, 1u);
		Owner.ReleaseResources();
		EXPECT_EQ(Second.GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_FALSE(Second.GetResult().Compute);
	}

	TEST_F(FRHIPipelineCreationTests, UniqueWorkSaturationDoesNotLimitSharedHandles)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
		std::promise<void> Entered, Release;
		auto EnteredFuture = Entered.get_future();
		auto Released = Release.get_future().share();
		std::atomic<uint32> Calls = 0;
		FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			if (++Calls == 1) { Entered.set_value(); Released.wait(); }
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		std::vector<FRHIPipelineCreationRequest> Requests;
		Requests.push_back(Owner.RequestCompute(Initializer, "first"));
		EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		for (uint32 Index = 1; Index < 256; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			Requests.push_back(Owner.RequestCompute(Initializer, "queued"));
			EXPECT_TRUE(Requests.back().IsAccepted());
		}
		Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 256;
		EXPECT_EQ(Owner.RequestCompute(Initializer, "overflow").GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
		Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 0;
		const auto PendingBytes = Owner.GetStatistics().UnfinishedDescriptionBytes;
		while (Requests.size() < 5000) Requests.push_back(Owner.RequestCompute(Initializer, "shared"));
		EXPECT_TRUE(Requests.back().IsAccepted());
		EXPECT_EQ(Requests.back(), Requests.front());
		EXPECT_EQ(Requests.back().GetTaskHandle().GetTaskId(), Requests.front().GetTaskHandle().GetTaskId());
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 256u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, PendingBytes);
		Release.set_value();
		for (auto& Request : Requests) EXPECT_TRUE(Request.Wait());
		EXPECT_EQ(Calls, 256u);
		Owner.ReleaseResources();
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 0u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, SharedDescriptionCapacityRejectsAndRecoversAfterRelease)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
		auto Caps = Capabilities();
		std::vector<FComputePipelineStateKey> Keys;
		bool Rejected = false;
		for (uint32 Index = 0; Index <= 4096; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			auto Key = BuildComputePipelineStateKey(Initializer, &Caps);
			if (!Key)
			{
				EXPECT_EQ(Key.error(), ERHIComputePipelineError::CapacityExceeded);
				Rejected = true;
				break;
			}
			Keys.push_back(std::move(*Key));
		}
		ASSERT_TRUE(Rejected);
		ASSERT_FALSE(Keys.empty());
		auto Create = [](const auto&, const auto&) { return MakeRefCount<FRHIComputePipelineState>(); };
		FPipelineCompileQueue Queue(Caps, Backend(Create));
		FRHIPipelineStateCache Cache(Caps, Backend(Create));
		EXPECT_EQ(Queue.RequestCompute(Initializer, "full").GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
		EXPECT_EQ(Cache.GetCompute(Initializer, "full").error(), ERHIPipelineRequestRejection::CapacityExceeded);
		Keys.clear();
		auto Request = Queue.RequestCompute(Initializer, "retry");
		ASSERT_TRUE(Request.IsAccepted());
		EXPECT_TRUE(Request.Wait());
		auto Cached = Cache.GetCompute(Initializer, "retry");
		ASSERT_TRUE(Cached);
		EXPECT_TRUE((*Cached)->Wait());
	}

	TEST_F(FRHIPipelineCreationTests, CompiledInputsReleaseBudgetWhileSharedHandlesRetainLayout)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		FRHIPipelineMetadataBudget Budget;
		std::promise<void> Release;
		auto Released = Release.get_future().share();
		auto Callbacks = Backend([&](const auto&, const auto&) {
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		});
		Callbacks.ReserveMetadata = [&](uint64 Bytes) { return Budget.Reserve(Bytes); };
		FPipelineCompileQueue Owner(Capabilities(), std::move(Callbacks));
		auto Request = Owner.RequestCompute(Initializer, std::string(64 * 1024, 'x'));
		ASSERT_TRUE(Request.IsAccepted());
		EXPECT_GT(Budget.GetUsedBytes(), 64 * 1024u);
		auto Layout = Request.GetPipelineLayout();
		ASSERT_TRUE(Layout);
		Release.set_value();
		EXPECT_TRUE(Request.Wait());
		Owner.StopAndWait();
		EXPECT_LT(Budget.GetUsedBytes(), 1024u);
		EXPECT_GT(Budget.GetUsedBytes(), 0u);
		Request = {};
		EXPECT_EQ(Budget.GetUsedBytes(), 0u);
		EXPECT_TRUE(Layout->BindingLayouts.empty());
	}

	TEST_F(FRHIPipelineCreationTests, UnfinishedPayloadBudgetRejectsBeforeRequestCountLimit)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
		std::promise<void> Release;
		auto Released = Release.get_future().share();
		FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		std::vector<FRHIPipelineCreationRequest> Requests;
		const std::string Name(900 * 1024, 'x');
		for (uint32 Index = 0; Index < 32; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			auto Request = Owner.RequestCompute(Initializer, Name);
			if (!Request.IsAccepted())
			{
				EXPECT_EQ(Request.GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
				break;
			}
			Requests.push_back(Request);
		}
		EXPECT_GT(Requests.size(), 0u);
		EXPECT_LT(Requests.size(), 32u);
		EXPECT_LE(Owner.GetStatistics().UnfinishedDescriptionBytes, 16ull * 1024 * 1024);
		Release.set_value();
		for (const auto& Request : Requests) EXPECT_TRUE(Request.Wait());
		Owner.ReleaseResources();
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, 0u);
	}


	TEST_F(FRHIPipelineCreationTests, MixedBatchPreservesReadyResultsAndRejectsInvalidDescriptions)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		std::atomic<bool> Fail = true;
		FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto& Inputs, const auto&) -> FComputePipelineStateRHIRef {
										(void)Inputs;
										return MakeRefCount<FRHIComputePipelineState>();
									}));
		std::vector<FRHIComputePipelineBatchItem> Items(3);
		for (uint32 Index = 0; Index < Items.size(); ++Index)
		{
			Items[Index].Initializer.ComputeShader = Shader;
			Items[Index].Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, Index, ERHIBindingType::UniformBuffer);
		}
		Items[1].DebugName = "fail";
		Items[2].Initializer.ComputeShader = nullptr;
		auto Batch = Owner.RequestComputeBatch(Items);
		ASSERT_EQ(Batch.Items.size(), 3u);
		EXPECT_TRUE(Batch.Items[0].Wait());
		EXPECT_TRUE(Batch.Items[1].Wait());
		EXPECT_EQ(Batch.Items[2].GetRejection(), ERHIPipelineRequestRejection::InvalidDescription);
		EXPECT_TRUE(Owner.RequestCompute(Items[1].Initializer, "retry").Wait());
		Items.resize(257);
		EXPECT_EQ(Owner.RequestComputeBatch(Items).Rejection, ERHIPipelineRequestRejection::CapacityExceeded);
		EXPECT_EQ(Owner.RequestCompute(Items[0].Initializer, std::string(1024 * 1024, 'x')).GetRejection(),
			ERHIPipelineRequestRejection::CapacityExceeded);
	}

	TEST_F(FRHIPipelineCreationTests, AcceptedCompilationFailureIsSharedWithoutAutomaticRetry)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) -> FComputePipelineStateRHIRef {
										 ++Calls;
										 return nullptr;
									 }));
		auto First = Owner.GetCompute(Initializer, "compile failure");
		ASSERT_TRUE(First);
		EXPECT_FALSE((*First)->Wait());
		EXPECT_EQ((*First)->GetState(), ERHIPipelineRequestState::Failed);
		EXPECT_FALSE((*First)->GetRHIPipeline());
		auto Again = Owner.GetCompute(Initializer, "same failed compilation");
		ASSERT_TRUE(Again);
		EXPECT_EQ(*First, *Again);
		EXPECT_FALSE((*Again)->Wait());
		EXPECT_EQ(Calls, 1u);
		Owner.ReleaseResources();
		EXPECT_EQ((*First)->GetState(), ERHIPipelineRequestState::Canceled);
	}

	TEST_F(FRHIPipelineCreationTests, UnexpectedExceptionsRemainTerminalFromBackgroundCreator)
	{
		const auto Run = [](int Mode) {
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			FPipelineCompileQueue Owner(Capabilities(), Backend([Mode](const auto&, const auto&) -> FComputePipelineStateRHIRef {
											if (Mode == 1) throw std::bad_alloc();
											if (Mode == 2) throw std::logic_error("terminal native invariant");
											return {};
										}));
			auto Request = Owner.RequestCompute(Initializer, "terminal");
			(void)Request.Wait();
		};
		EXPECT_DEATH(Run(1), "");
		EXPECT_DEATH(Run(2), "");
	}

	TEST_F(FRHIPipelineCreationTests, CloseSettlesQueuedWorkAndJoinsActiveCompilationBeforeRetirement)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		std::promise<void> Entered, Release;
		auto EnteredFuture = Entered.get_future();
		auto Released = Release.get_future().share();
		std::atomic<bool> Exited = false;
		FRHIPipelineCreationRequest Request;
		{
			FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) {
				Entered.set_value();
				Released.wait();
				Exited = true;
				return MakeRefCount<FRHIComputePipelineState>();
			}));
			Request = Owner.RequestCompute(Initializer, "close in flight");
			EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			Initializer.PipelineLayout.PushConstantRanges.push_back({EShaderStageFlags::Compute, 0, 4});
			auto Queued = Owner.RequestCompute(Initializer, "never started");
			auto ConcurrentStop = std::async(std::launch::async, [&] { Owner.StopAndWait(); });
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!Owner.IsClosed() || !Queued.GetTaskHandle().IsCompleted())
				&& std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			EXPECT_TRUE(Owner.IsClosed());
			EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Pending);
			EXPECT_FALSE(Request.GetTaskHandle().IsCompleted());
			EXPECT_FALSE(Exited.load());
			// Queued work can settle immediately; the active compile must retire first.
			EXPECT_EQ(Queued.GetState(), ERHIPipelineRequestState::Canceled);
			Release.set_value();
			Owner.ReleaseResources();
			ConcurrentStop.get();
			EXPECT_TRUE(Exited.load());
			EXPECT_EQ(Owner.RequestCompute(Initializer, "closed").GetRejection(), ERHIPipelineRequestRejection::Closed);
		}
		EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_TRUE(Request.GetTaskHandle().IsCompleted());
	}
}

TEST(RHICreationTerminalTests, ExitsWithoutCoreOrTeardown)
{
	EXPECT_EXIT(Durin::RHITerminateOnOutOfMemory("startup texture", -2), testing::ExitedWithCode(EXIT_FAILURE), "startup texture: out of memory.*native=-2");
}

TEST(RHICreationTerminalTests, ReturnedFatalFailureUsesProcessTerminatePolicy)
{
	EXPECT_EXIT(([] {
					std::set_terminate([] { std::_Exit(42); });
					Durin::RHITerminateOnFailure("pipeline creation", "device lost", -4);
				})(),
				testing::ExitedWithCode(42), "pipeline creation: device lost.*native=-4");
}

namespace Durin
{
    TEST_F(FRHIPipelineCreationTests, OutcomeMetadataSurvivesSharedStateAndNativeResourceRetirement)
    {
        std::atomic<uint32> Destroyed = 0;
        class FObservedPipeline final : public FRHIComputePipelineState
        {
            std::atomic<uint32>& Destroyed;
        public:
            explicit FObservedPipeline(std::atomic<uint32>& InDestroyed) : Destroyed(InDestroyed) {}
            ~FObservedPipeline() override { ++Destroyed; }
        };
        std::shared_ptr<const std::atomic<ERHIPipelineRequestState>> Outcome;
        Tasks::FTask Completion;
        {
            FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) -> FComputePipelineStateRHIRef {
                return MakeRefCount<FObservedPipeline>(Destroyed);
            }));
            auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
            FComputePipelineStateInitializer Initializer; Initializer.ComputeShader = Shader;
            auto Request = Owner.RequestCompute(Initializer, "outcome lifetime");
            ASSERT_TRUE(Request.IsAccepted()); Outcome = Request.GetStateObservation();
            Completion = Request.GetTaskHandle();
            ASSERT_TRUE(Request.Wait());
            EXPECT_EQ(Outcome->load(), ERHIPipelineRequestState::Ready);
            Request = {};
            Owner.ReleaseResources();
        }
        RHIFlushDeferredResources();
        EXPECT_EQ(Destroyed.load(), 1u);
        EXPECT_TRUE(Completion.IsCompleted());
        EXPECT_EQ(Outcome->load(), ERHIPipelineRequestState::Ready);
    }
}
