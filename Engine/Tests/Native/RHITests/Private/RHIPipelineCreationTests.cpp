#include <gtest/gtest.h>
#include <future>

#include "RHIPipelineCreation.h"
#include "PipelineStateCache.h"
#include "PipelineCompileQueue.h"
#include "RHICapabilities.h"
#include "RHICommandList.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"

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
				.PublishTerminalFailure = [](std::exception_ptr) { ADD_FAILURE() << "Unexpected terminal creation failure."; }
			};
		}
	};

	TEST_F(FRHIPipelineCreationTests, CacheSharesPendingIdentityWithoutSharingCancellationAuthority)
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
		auto First = Cache.GetCompute(Initializer, "precache");
		auto Second = Cache.GetCompute(Initializer, "draw");
		EXPECT_TRUE(First);
		EXPECT_TRUE(Second);
		if (First && Second)
		{
			EXPECT_EQ(*First, *Second);
			EXPECT_EQ((*First)->GetState(), ERHIPipelineRequestState::Pending);
			EXPECT_EQ((*First)->GetPipelineLayout()->PushConstantRanges.size(), 1u);
			EXPECT_FALSE((*First)->GetRHIPipeline());
			EXPECT_EQ(Owner.GetStatistics().ActiveObservers, 1u);
			First = std::unexpected(ERHIPipelineRequestRejection::Unsupported);
			EXPECT_EQ((*Second)->GetState(), ERHIPipelineRequestState::Pending);
		}
		Release.set_value();
		if (Second) EXPECT_TRUE((*Second)->Wait());
		EXPECT_EQ(Calls, 1u);
	}

	TEST_F(FRHIPipelineCreationTests, CacheColdMissFromForeignScopeDoesNotWaitForNativeCreation)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		std::promise<void> Release;
		auto Released = Release.get_future().share();
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto Scope = CreateTaskScope();
		Tasks::FTaskGroup Group(Scope.GetToken());
		std::promise<FComputePipelineStateRef> Submitted;
		auto SubmittedFuture = Submitted.get_future();
		auto Producer = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
			auto Cached = Owner.GetCompute(Initializer, "parallel draw");
			EXPECT_TRUE(Cached);
			if (Cached)
			{
				EXPECT_EQ((*Cached)->GetState(), ERHIPipelineRequestState::Pending);
				EXPECT_FALSE((*Cached)->CanWait());
				EXPECT_FALSE((*Cached)->Wait());
				Submitted.set_value(*Cached);
			}
			else Submitted.set_value({});
		});
		const auto Admission = SubmittedFuture.wait_for(std::chrono::seconds(5));
		EXPECT_EQ(Admission, std::future_status::ready);
		// Always release the creator before asserting or joining scopes.
		Release.set_value();
		auto Pipeline = SubmittedFuture.get();
		if (Pipeline)
		{
			EXPECT_TRUE(Pipeline->Wait());
			auto Hit = Owner.GetCompute(Initializer, "owner hit");
			EXPECT_TRUE(Hit);
			if (Hit) EXPECT_EQ(*Hit, Pipeline);
		}
		Group.Close();
		EXPECT_EQ(Scope.Wait(), ETaskScopeWaitResult::Quiescent);
	}

	TEST_F(FRHIPipelineCreationTests, CacheFailedIdentityRetriesAndDeviceRetirementInvalidatesSurvivingHandles)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		std::atomic<uint32> Calls = 0;
		FRHIPipelineStateCache Owner(Capabilities(), Backend([&](const auto&, const auto&) {
			if (++Calls == 1) throw FRHIRecoverableCreationError({ERHIResourceCreationFailure::OutOfMemory, ERHICreationFailureSource::NativeBackend, -7});
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto& Cache = Owner;
		auto Failed = Cache.GetCompute(Initializer, "failure");
		ASSERT_TRUE(Failed);
		EXPECT_FALSE((*Failed)->Wait());
		EXPECT_EQ((*Failed)->GetCreationError().NativeCode, -7);
		auto Retried = Cache.GetCompute(Initializer, "retry");
		ASSERT_TRUE(Retried);
		EXPECT_NE(*Failed, *Retried);
		EXPECT_TRUE((*Retried)->Wait());
		EXPECT_TRUE((*Retried)->GetRHIPipeline());
		EXPECT_EQ(Calls, 2u);
		Owner.CloseAndJoin(false);
		EXPECT_EQ((*Retried)->GetState(), ERHIPipelineRequestState::Ready);
		EXPECT_EQ(Cache.GetCompute(Initializer, "closed").error(), ERHIPipelineRequestRejection::Closed);
		Owner.CloseAndJoin();
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
				throw FRHIRecoverableCreationError({ERHIResourceCreationFailure::ResourceExhausted, ERHICreationFailureSource::MetadataBudget});
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
		Owner.CloseAndJoin();
		Second = std::unexpected(ERHIPipelineRequestRejection::Closed);
		EXPECT_EQ(CacheLeases, 0u);
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
		EXPECT_EQ(Calls, 0u);
		EXPECT_EQ(Owner.GetStatistics().ActiveObservers, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, SharesWorkAndOwnsInputsWhileObserversCancelIndependently)
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
			EXPECT_EQ(Key.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot, 3u);
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		auto First = Owner.RequestCompute(Initializer, Name);
		auto Second = Owner.RequestCompute(Initializer, "different diagnostic name");
		EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		Initializer.PipelineLayout.BindingLayouts.clear();
		Initializer.ComputeShader = nullptr;
		Shader = nullptr;
		Name.assign("modified");
		EXPECT_TRUE(First.Cancel());
		EXPECT_EQ(First.GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_EQ(Second.GetState(), ERHIPipelineRequestState::Pending);
		Release.set_value();
		EXPECT_TRUE(Second.Wait());
		EXPECT_TRUE(Second.GetResult().Compute);
		EXPECT_FALSE(Second.Cancel());
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Owner.GetStatistics().SharedPendingHits, 1u);
		Owner.CloseAndJoin();
		EXPECT_EQ(Second.GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_FALSE(Second.GetResult().Compute);
	}

	TEST_F(FRHIPipelineCreationTests, SaturationRejectsAndCancelingHandlesDoesNotReleaseLiveObserverBudget)
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
		while (Requests.size() < 4096) Requests.push_back(Owner.RequestCompute(Initializer, "shared"));
		EXPECT_TRUE(Requests.back().IsAccepted());
		EXPECT_EQ(Owner.GetStatistics().ActiveObservers, 4096u);
		EXPECT_TRUE(Requests.back().Cancel());
		EXPECT_EQ(Owner.RequestCompute(Initializer, "still full").GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
		Requests.pop_back();
		Requests.push_back(Owner.RequestCompute(Initializer, "reused observer slot"));
		EXPECT_TRUE(Requests.back().IsAccepted());
		Release.set_value();
		for (auto& Request : Requests) EXPECT_TRUE(Request.Wait());
		EXPECT_EQ(Calls, 256u);
		Owner.CloseAndJoin();
		EXPECT_EQ(Owner.GetStatistics().UnfinishedRequests, 0u);
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, 0u);
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
		for (uint32 Index = 0; Index < 16; ++Index)
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
		EXPECT_LT(Requests.size(), 16u);
		EXPECT_LE(Owner.GetStatistics().UnfinishedDescriptionBytes, 16ull * 1024 * 1024);
		Release.set_value();
		for (const auto& Request : Requests) EXPECT_TRUE(Request.Wait());
		Owner.CloseAndJoin();
		EXPECT_EQ(Owner.GetStatistics().UnfinishedDescriptionBytes, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, ForeignTaskScopesRejectBeforeCoreConstructionWithoutStarvingWorker)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		FPipelineCompileQueue Owner(Capabilities(), Backend([](const auto&, const auto&) {
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		std::promise<FRHIPipelineCreationRequest> Submitted;
		auto SubmittedFuture = Submitted.get_future();
		auto Scope = CreateTaskScope();
		Tasks::FTaskGroup Group(Scope.GetToken());
		auto Producer = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
			auto Request = Owner.RequestCompute(Initializer, "single worker caller");
			EXPECT_FALSE(Request.Wait());
			EXPECT_EQ(Request.GetRejection(), ERHIPipelineRequestRejection::Unsupported);
			Submitted.set_value(Request);
		});
		EXPECT_EQ(SubmittedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		auto Request = SubmittedFuture.get();
		EXPECT_FALSE(Request.IsAccepted());
		Request = Owner.RequestCompute(Initializer, "owning caller");
		EXPECT_TRUE(Request.Wait());
		auto Completion = Request.GetCompletion();
		Request = {};
		Owner.CloseAndJoin();
		EXPECT_EQ(Owner.GetStatistics().ActiveObservers, 0u);
		EXPECT_TRUE(Completion.IsReady());
		Group.Close();
		EXPECT_EQ(Scope.Wait(), ETaskScopeWaitResult::Quiescent);
	}

	TEST_F(FRHIPipelineCreationTests, MixedBatchPreservesPartialResultsAndRecoverableFailuresCanRetry)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		std::atomic<bool> Fail = true;
		FPipelineCompileQueue Owner(Capabilities(), Backend([&](const auto& Inputs, const auto&) {
			if (Inputs.DebugName == "fail" && Fail.exchange(false)) throw FRHIRecoverableCreationError({ERHIResourceCreationFailure::ResourceExhausted, ERHICreationFailureSource::MetadataBudget});
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
		EXPECT_FALSE(Batch.Items[1].Wait());
		EXPECT_EQ(Batch.Items[1].GetResult().Error.Failure, ERHIResourceCreationFailure::ResourceExhausted);
		EXPECT_EQ(Batch.Items[1].GetResult().Error.Source, ERHICreationFailureSource::MetadataBudget);
		EXPECT_EQ(Batch.Items[2].GetRejection(), ERHIPipelineRequestRejection::InvalidDescription);
		EXPECT_TRUE(Owner.RequestCompute(Items[1].Initializer, "retry").Wait());
		Items.resize(257);
		EXPECT_EQ(Owner.RequestComputeBatch(Items).Rejection, ERHIPipelineRequestRejection::CapacityExceeded);
		EXPECT_EQ(Owner.RequestCompute(Items[0].Initializer, std::string(1024 * 1024, 'x')).GetRejection(),
			ERHIPipelineRequestRejection::CapacityExceeded);
	}

	TEST_F(FRHIPipelineCreationTests, TerminalErrorPublishesBeforeObserversFinishAndPreservesException)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		std::atomic<bool> Published = false;
		auto Callbacks = Backend([](const auto&, const auto&) -> FComputePipelineStateRHIRef {
			throw std::logic_error("terminal native invariant");
		});
		Callbacks.PublishTerminalFailure = [&](std::exception_ptr Error) {
			EXPECT_THROW(std::rethrow_exception(Error), std::logic_error);
			Published = true;
		};
		FPipelineCompileQueue Owner(Capabilities(), std::move(Callbacks));
		auto Request = Owner.RequestCompute(Initializer, "terminal");
		EXPECT_FALSE(Request.Wait());
		EXPECT_TRUE(Published.load());
		EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Failed);
		EXPECT_THROW(Request.GetResult(), std::logic_error);
		EXPECT_EQ(Owner.RequestCompute(Initializer, "late").GetRejection(), ERHIPipelineRequestRejection::Closed);
		Owner.CloseAndJoin();
		EXPECT_TRUE(Request.GetCompletion().IsReady());
	}

	TEST_F(FRHIPipelineCreationTests, CloseCancelsObserversAndJoinsInFlightCreation)
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
			auto ReleaseAfterCancel = std::async(std::launch::async, [&] {
				EXPECT_FALSE(Request.Wait());
				EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Canceled);
				Release.set_value();
			});
			Owner.CloseAndJoin();
			ReleaseAfterCancel.get();
			EXPECT_TRUE(Exited.load());
			EXPECT_EQ(Owner.RequestCompute(Initializer, "closed").GetRejection(), ERHIPipelineRequestRejection::Closed);
		}
		EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_TRUE(Request.GetCompletion().IsReady());
	}
}
