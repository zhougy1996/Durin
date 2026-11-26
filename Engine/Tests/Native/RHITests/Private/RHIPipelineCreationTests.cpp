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

	TEST_F(FRHIPipelineCreationTests, CompilationWaitObservesReadyOrFailedNativeOutcome)
	{
		for (const auto Expected : {ERHIPipelineState::Ready, ERHIPipelineState::Failed})
		{
			SCOPED_TRACE(static_cast<int>(Expected));
			std::promise<void> Release;
			auto Released = Release.get_future().share();
			FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) -> FComputePipelineStateRHIRef {
				Released.wait();
				return Expected == ERHIPipelineState::Ready ? MakeRefCount<FRHIComputePipelineState>() : nullptr;
			}));
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			auto Request = Owner.RequestCompute(Initializer, "shared outcome");
			auto Peer = Owner.RequestCompute(Initializer, "same compilation");
			EXPECT_EQ(Request, Peer);
			EXPECT_EQ(Request.GetStateObservation(), Peer.GetStateObservation());
			auto Waiting = std::async(std::launch::async, [&] {
				return Peer.Wait();
			});
			Release.set_value();
			EXPECT_EQ(Waiting.get(), Expected);
			EXPECT_EQ(FRHIPipelineStateCacheBackend::GetComputePipeline(Peer) != nullptr,
				Expected == ERHIPipelineState::Ready);
			// Joining after publication must return the same result immediately.
			EXPECT_EQ(Request.Wait(), Expected);
			Owner.Shutdown();
		}
	}

	TEST_F(FRHIPipelineCreationTests, ConcurrentCompilationWaitAndPublicationCannotLoseCompletion)
	{
		for (int Iteration = 0; Iteration < 32; ++Iteration)
		{
			std::promise<void> Start;
			auto Gate = Start.get_future().share();
			FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) {
				Gate.wait();
				return MakeRefCount<FRHIComputePipelineState>();
			}));
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			auto Request = Owner.RequestCompute(Initializer, "wake race");
			auto Waiter = std::async(std::launch::async, [&] {
				Gate.wait();
				return Request.Wait();
			});
			Start.set_value();
			EXPECT_EQ(Waiter.get(), ERHIPipelineState::Ready);
			Owner.Shutdown();
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
		ASSERT_TRUE(Request.IsValid());
		Entered.get_future().wait();
		auto Signal = Request.GetStateObservation();
		Request = {};
		EXPECT_EQ(Signal.GetState(), ERHIPipelineState::Pending);
		auto Replacement = Owner.RequestCompute(Initializer, "reattach to shared compilation");
		EXPECT_EQ(Replacement.GetStateObservation(), Signal);
		Replacement = {};
		EXPECT_FALSE(Retired.load());
		Release.set_value(); Owner.Shutdown();
		EXPECT_NE(Signal.GetState(), ERHIPipelineState::Pending);
		EXPECT_TRUE(Retired.load());
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
		EXPECT_EQ(First, Peer);
		Peer = {};
		Release.set_value();
		EXPECT_EQ(Active.Wait(), ERHIPipelineState::Ready); EXPECT_EQ(First.Wait(), ERHIPipelineState::Ready); EXPECT_EQ(Second.Wait(), ERHIPipelineState::Ready); EXPECT_EQ(Medium.Wait(), ERHIPipelineState::Ready);
		Queue.Shutdown();
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
			EXPECT_EQ(First, Second);
			EXPECT_EQ(First->GetState(), ERHIPipelineState::Pending);
			EXPECT_EQ(First->GetPipelineLayout()->PushConstantRanges.size(), 1u);
			EXPECT_FALSE(First->GetRHIPipeline());
			EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 1u);
			First = {};
			EXPECT_EQ(Second->GetState(), ERHIPipelineState::Pending);
		}
		Release.set_value();
		if (Second) EXPECT_EQ(Second->Wait(), ERHIPipelineState::Ready);
		EXPECT_EQ(Calls, 1u);
	}


	TEST_F(FRHIPipelineCreationTests, CacheSharesReadyIdentityAndPreservesCompilationOutcomeOnClose)
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
		EXPECT_EQ(Failed->Wait(), ERHIPipelineState::Ready);
		EXPECT_TRUE(Failed->GetRHIPipeline());
		auto Retried = Cache.GetCompute(Initializer, "retry");
		ASSERT_TRUE(Retried);
		EXPECT_EQ(Failed, Retried);
		EXPECT_EQ(Retried->Wait(), ERHIPipelineState::Ready);
		EXPECT_TRUE(Retried->GetRHIPipeline());
		EXPECT_EQ(Calls, 1u);
		const auto Outcome = Retried->GetStateObservation();
		Owner.Shutdown();
		Owner.Shutdown();
		EXPECT_EQ(Retried->GetState(), ERHIPipelineState::Ready);
		EXPECT_FALSE(Cache.GetCompute(Initializer, "closed"));
		Failed.reset();
		Retried.reset();
		EXPECT_EQ(Outcome.GetState(), ERHIPipelineState::Ready);
	}

	TEST_F(FRHIPipelineCreationTests, MetadataAccountingDoesNotLimitAdmissionAndOutlivesTracker)
	{
		std::shared_ptr<void> Lease;
		{
			FRHIPipelineMetadataTracker Metadata;
			Lease = Metadata.Reserve(128ull * 1024 * 1024);
			EXPECT_EQ(Metadata.GetUsedBytes(), 128ull * 1024 * 1024);
			auto Second = Metadata.Reserve(1);
			EXPECT_EQ(Metadata.GetUsedBytes(), 128ull * 1024 * 1024 + 1);
			Second.reset();
			EXPECT_EQ(Metadata.GetUsedBytes(), 128ull * 1024 * 1024);
		}
		Lease.reset();
	}

	TEST_F(FRHIPipelineCreationTests, ColdWorkerAdmissionStaysOnCallerAndDoesNotNestCompilation)
	{
		std::promise<void> Release;
		auto Released = Release.get_future().share();
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
				EXPECT_EQ(Result->GetState(), ERHIPipelineState::Pending);
				EXPECT_EQ(Result->Wait(), ERHIPipelineState::Pending);
			}
			return Result;
		});
		// Poll without retracting the parent onto the calling thread.
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!Parent.IsCompleted() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		EXPECT_TRUE(Parent.IsCompleted());
		EXPECT_EQ(AdmissionTask.load(), Parent.GetTaskId());
		Release.set_value();
		ASSERT_TRUE(Parent.Wait(5));
		const auto& Result = Parent.GetResult();
		ASSERT_TRUE(Result);
		EXPECT_EQ(Result->Wait(), ERHIPipelineState::Ready);
	}

	TEST_F(FRHIPipelineCreationTests, QueueAdmitsOrdinaryTasksButRejectsIndependentCpuWork)
	{
		FPipelineCompileQueue Owner(Capabilities(), Backend([](const auto&, const auto&) {
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		for (bool BlockingIO : {false, true})
		{
			auto RequestPipeline = [&] {
				return Owner.RequestCompute(Initializer, "task PSO");
			};
			auto Task = BlockingIO ? Tasks::LaunchBlockingIO("direct queue admission", RequestPipeline)
				: Tasks::LaunchTask("direct queue admission", RequestPipeline);
			ASSERT_TRUE(Task.Wait(5));
			const auto& Request = Task.GetResult();
			ASSERT_TRUE(Request.IsValid());
			EXPECT_EQ(Request.Wait(), ERHIPipelineState::Ready);
		}
		auto Independent = Tasks::LaunchIndependentTask("direct queue CPU rejection", [&] {
			return Owner.RequestCompute(Initializer, "independent PSO");
		});
		EXPECT_FALSE(Independent.GetResult());
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
		using TResult = FComputePipelineStateRef;
		std::vector<Tasks::TTask<TResult>> Admissions;
		for (uint32 Index = 0; Index < 8; ++Index)
			Admissions.push_back(Tasks::LaunchTask("concurrent PSO admission", [&] { return Owner.GetCompute(Initializer, "shared PSO"); }));
		std::vector<FComputePipelineStateRef> Pipelines;
		for (const auto& Task : Admissions)
		{
			const bool Completed = Task.Wait(5);
			EXPECT_TRUE(Completed);
			if (Completed && Task.GetResult()) Pipelines.push_back(Task.GetResult());
		}
		Release.set_value();
		ASSERT_EQ(Pipelines.size(), Admissions.size());
		for (const auto& Pipeline : Pipelines)
		{
			EXPECT_EQ(Pipeline, Pipelines.front());
			EXPECT_EQ(Pipeline->Wait(), ERHIPipelineState::Ready);
		}
		EXPECT_EQ(Calls.load(), 1u);
		auto ConcurrentStop = std::async(std::launch::async, [&] { Owner.Shutdown(); });
		Owner.Shutdown();
		ConcurrentStop.get();
		EXPECT_FALSE(Owner.GetCompute(Initializer, "closed"));
		EXPECT_EQ(Pipelines.front()->GetState(), ERHIPipelineState::Ready);
		Pipelines.clear();
	}

	TEST_F(FRHIPipelineCreationTests, CacheRejectsInvalidDescriptionsAndIndependentCpuMisses)
	{
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			++Calls;
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		FComputePipelineStateInitializer Initializer;
		EXPECT_FALSE(Owner.GetCompute(Initializer, "invalid"));
		FGraphicsPipelineStateInitializer GraphicsInitializer;
		EXPECT_FALSE(Owner.GetGraphics(GraphicsInitializer, "invalid graphics"));
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		Initializer.ComputeShader = Shader;
		auto Task = Tasks::LaunchIndependentTask("cache leaf rejection", [&] {
			return Owner.GetCompute(Initializer, "cpu leaf");
		});
		EXPECT_FALSE(Task.GetResult());
		auto Parent = Tasks::LaunchIndependentTask("cache inherited CPU rejection", [&] {
			auto Child = Tasks::LaunchTask("cache inherited CPU child", [&] {
				return Owner.GetCompute(Initializer, "cpu child");
			});
			return Child.GetResult();
		});
		EXPECT_FALSE(Parent.GetResult());
		EXPECT_EQ(Calls, 0u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, IndependentRecordingCanLookUpPendingIdentityWithoutJoiningCompilation)
	{
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
		auto Required = Owner.GetCompute(Initializer, "required before recording");
		auto Recording = Tasks::LaunchIndependentTask("pending identity lookup", [&] {
			return Owner.GetCompute(Initializer, "recording lookup");
		});
		EXPECT_EQ(Recording.GetResult(), Required);
		if (Required) EXPECT_EQ(Required->GetState(), ERHIPipelineState::Pending);
		Release.set_value();
		ASSERT_TRUE(Required);
		EXPECT_EQ(Required->Wait(), ERHIPipelineState::Ready);
		EXPECT_EQ(Calls, 1u);
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
		auto LargeName = FRHIPipelineStateCacheBackend::GetComputeValidated(Owner, Initializer,
			std::string(1024 * 1024, 'x'), *Key);
		ASSERT_TRUE(LargeName);
		EXPECT_EQ(LargeName->Wait(), ERHIPipelineState::Ready);
		auto Task = Tasks::LaunchIndependentTask("validated cache identity lookup", [&] {
			return FRHIPipelineStateCacheBackend::GetComputeValidated(Owner, Initializer, "cpu leaf", *Key);
		});
		EXPECT_EQ(Task.GetResult(), LargeName);
		Owner.Shutdown();
		EXPECT_FALSE(FRHIPipelineStateCacheBackend::GetComputeValidated(Owner, Initializer,
			"closed", *Key));
		EXPECT_EQ(Calls, 1u);
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
		EXPECT_EQ(First, Second);
		First = {};
		EXPECT_EQ(Second.GetState(), ERHIPipelineState::Pending);
		Release.set_value();
		EXPECT_EQ(Second.Wait(), ERHIPipelineState::Ready);
		EXPECT_TRUE(FRHIPipelineStateCacheBackend::GetComputePipeline(Second));
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Owner.GetStatistics().SharedPendingHits, 1u);
		Owner.Shutdown();
		EXPECT_EQ(Second.GetState(), ERHIPipelineState::Ready);
	}

	TEST_F(FRHIPipelineCreationTests, LargePendingQueueSharesHandlesAndSerializesNativeCompilation)
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
		std::vector<FPipelineCompilation> Requests;
		Requests.push_back(Owner.RequestCompute(Initializer, "first"));
		EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		for (uint32 Index = 1; Index < 512; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			Requests.push_back(Owner.RequestCompute(Initializer, "queued"));
			EXPECT_TRUE(Requests.back().IsValid());
		}
		Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 0;
		const auto PendingBytes = Owner.GetStatistics().UnfinishedDescriptionBytes;
		while (Requests.size() < 5000) Requests.push_back(Owner.RequestCompute(Initializer, "shared"));
		EXPECT_TRUE(Requests.back().IsValid());
		EXPECT_EQ(Requests.back(), Requests.front());
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 512u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, PendingBytes);
		Release.set_value();
		for (auto& Request : Requests) EXPECT_EQ(Request.Wait(), ERHIPipelineState::Ready);
		EXPECT_EQ(Calls, 512u);
		Owner.Shutdown();
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 0u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, LiveKeysAndCacheIdentitiesExceedFormerCapacity)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
		auto Caps = Capabilities();
		std::vector<FComputePipelineStateKey> Keys;
		std::vector<FComputePipelineStateRef> Pipelines;
		FRHIPipelineStateCache Cache(Caps, Backend([](const auto&, const auto&) { return MakeRefCount<FRHIComputePipelineState>(); }));
		for (uint32 Index = 0; Index < 4100; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			auto Key = BuildComputePipelineStateKey(Initializer, &Caps);
			ASSERT_TRUE(Key);
			Keys.push_back(*Key);
			auto Cached = Cache.GetCompute(Initializer, "live identity");
			ASSERT_TRUE(Cached);
			Pipelines.push_back(Cached);
		}
		EXPECT_EQ(Keys.size(), 4100u);
		for (const auto& Pipeline : Pipelines) EXPECT_EQ(Pipeline->Wait(), ERHIPipelineState::Ready);
		auto Again = Cache.GetCompute(Initializer, "shared");
		ASSERT_TRUE(Again);
		EXPECT_EQ(Again, Pipelines.back());
	}

	TEST_F(FRHIPipelineCreationTests, CompiledInputsReleaseAccountingWhileSharedHandlesRetainLayout)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		FRHIPipelineMetadataTracker Budget;
		std::promise<void> Release;
		auto Released = Release.get_future().share();
		auto Callbacks = Backend([&](const auto&, const auto&) {
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		});
		Callbacks.ReserveMetadata = [&](uint64 Bytes) { return Budget.Reserve(Bytes); };
		FPipelineCompileQueue Owner(Capabilities(), std::move(Callbacks));
		auto Request = Owner.RequestCompute(Initializer, std::string(64 * 1024, 'x'));
		ASSERT_TRUE(Request.IsValid());
		EXPECT_GT(Budget.GetUsedBytes(), 64 * 1024u);
		auto Layout = Request.GetPipelineLayout();
		ASSERT_TRUE(Layout);
		Release.set_value();
		EXPECT_EQ(Request.Wait(), ERHIPipelineState::Ready);
		Owner.Shutdown();
		EXPECT_LT(Budget.GetUsedBytes(), 1024u);
		EXPECT_GT(Budget.GetUsedBytes(), 0u);
		Request = {};
		EXPECT_EQ(Budget.GetUsedBytes(), 0u);
		EXPECT_TRUE(Layout->BindingLayouts.empty());
	}

	TEST_F(FRHIPipelineCreationTests, LargePendingPayloadsRemainAccepted)
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
		std::vector<FPipelineCompilation> Requests;
		const std::string Name(900 * 1024, 'x');
		for (uint32 Index = 0; Index < 32; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			auto Request = Owner.RequestCompute(Initializer, Name);
			EXPECT_TRUE(Request.IsValid());
			Requests.push_back(Request);
		}
		EXPECT_EQ(Requests.size(), 32u);
		EXPECT_GT(Owner.GetStatistics().UnfinishedDescriptionBytes, 16ull * 1024 * 1024);
		Release.set_value();
		for (const auto& Request : Requests) EXPECT_EQ(Request.Wait(), ERHIPipelineState::Ready);
		Owner.Shutdown();
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
		EXPECT_EQ(Batch.Items[0].Wait(), ERHIPipelineState::Ready);
		EXPECT_EQ(Batch.Items[1].Wait(), ERHIPipelineState::Ready);
		EXPECT_FALSE(Batch.Items[2]);
		EXPECT_EQ(Owner.RequestCompute(Items[1].Initializer, "retry").Wait(), ERHIPipelineState::Ready);
		Items.resize(257);
		EXPECT_EQ(Owner.RequestComputeBatch(Items).Items.size(), 257u);
		EXPECT_EQ(Owner.RequestCompute(Items[0].Initializer, std::string(1024 * 1024, 'x')).Wait(), ERHIPipelineState::Ready);
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
		EXPECT_EQ(First->Wait(), ERHIPipelineState::Failed);
		EXPECT_EQ(First->GetState(), ERHIPipelineState::Failed);
		EXPECT_FALSE(First->GetRHIPipeline());
		auto Again = Owner.GetCompute(Initializer, "same failed compilation");
		ASSERT_TRUE(Again);
		EXPECT_EQ(First, Again);
		EXPECT_EQ(Again->Wait(), ERHIPipelineState::Failed);
		EXPECT_EQ(Calls, 1u);
		Owner.Shutdown();
		EXPECT_EQ(First->GetState(), ERHIPipelineState::Failed);
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
		FPipelineCompilation Request;
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
			auto QueuedWait = std::async(std::launch::async, [&] {
				return Queued.Wait();
			});
			auto ConcurrentStop = std::async(std::launch::async, [&] { Owner.Shutdown(); });
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!Owner.IsClosed() || Queued.GetState() == ERHIPipelineState::Pending)
				&& std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			EXPECT_TRUE(Owner.IsClosed());
			EXPECT_EQ(Request.GetState(), ERHIPipelineState::Pending);
			EXPECT_FALSE(Exited.load());
			// Queued work can settle immediately; the active compile must retire first.
			EXPECT_EQ(Queued.GetState(), ERHIPipelineState::Canceled);
			EXPECT_EQ(Queued.Wait(), ERHIPipelineState::Canceled);
			Release.set_value();
			Owner.Shutdown();
			ConcurrentStop.get();
			EXPECT_EQ(QueuedWait.get(), ERHIPipelineState::Canceled);
			EXPECT_TRUE(Exited.load());
			EXPECT_FALSE(Owner.RequestCompute(Initializer, "closed"));
		}
		EXPECT_EQ(Request.GetState(), ERHIPipelineState::Canceled);
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
        FRHIPipelineStateObservation Outcome;
        {
            FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto&, const auto&) -> FComputePipelineStateRHIRef {
                return MakeRefCount<FObservedPipeline>(Destroyed);
            }));
            auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
            FComputePipelineStateInitializer Initializer; Initializer.ComputeShader = Shader;
            auto Request = Owner.RequestCompute(Initializer, "outcome lifetime");
            ASSERT_TRUE(Request.IsValid()); Outcome = Request.GetStateObservation();
            ASSERT_EQ(Request.Wait(), ERHIPipelineState::Ready);
            EXPECT_EQ(Outcome.GetState(), ERHIPipelineState::Ready);
            Request = {};
            Owner.Shutdown();
        }
        RHIFlushDeferredResources();
        EXPECT_EQ(Destroyed.load(), 1u);
        EXPECT_EQ(Outcome.GetState(), ERHIPipelineState::Ready);
    }
}
