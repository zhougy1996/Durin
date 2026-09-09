#include <gtest/gtest.h>
#include <future>

#include "RHIPipelineCreation.h"
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
			-> FRHIPipelineCreationService::FBackend
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
		FRHIPipelineCreationService Service(Capabilities(), Backend([&](const auto& Inputs, const auto& Key) {
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
		auto First = Service.RequestCompute(Initializer, Name);
		auto Second = Service.RequestCompute(Initializer, "different diagnostic name");
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
		EXPECT_EQ(Service.GetStatistics().SharedPendingHits, 1u);
		Service.CloseAndJoin();
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
		FRHIPipelineCreationService Service(Capabilities(), Backend([&](const auto&, const auto&) {
			if (++Calls == 1) { Entered.set_value(); Released.wait(); }
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		std::vector<FRHIPipelineCreationRequest> Requests;
		Requests.push_back(Service.RequestCompute(Initializer, "first"));
		EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		for (uint32 Index = 1; Index < 256; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			Requests.push_back(Service.RequestCompute(Initializer, "queued"));
			EXPECT_TRUE(Requests.back().IsAccepted());
		}
		Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 256;
		EXPECT_EQ(Service.RequestCompute(Initializer, "overflow").GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
		Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 0;
		while (Requests.size() < 4096) Requests.push_back(Service.RequestCompute(Initializer, "shared"));
		EXPECT_TRUE(Requests.back().IsAccepted());
		EXPECT_EQ(Service.GetStatistics().ActiveObservers, 4096u);
		EXPECT_TRUE(Requests.back().Cancel());
		EXPECT_EQ(Service.RequestCompute(Initializer, "still full").GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
		Requests.pop_back();
		Requests.push_back(Service.RequestCompute(Initializer, "reused observer slot"));
		EXPECT_TRUE(Requests.back().IsAccepted());
		Release.set_value();
		for (auto& Request : Requests) EXPECT_TRUE(Request.Wait());
		EXPECT_EQ(Calls, 256u);
		Service.CloseAndJoin();
		EXPECT_EQ(Service.GetStatistics().UnfinishedRequests, 0u);
		EXPECT_EQ(Service.GetStatistics().UnfinishedDescriptionBytes, 0u);
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
		FRHIPipelineCreationService Service(Capabilities(), Backend([&](const auto&, const auto&) {
			Released.wait();
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		std::vector<FRHIPipelineCreationRequest> Requests;
		const std::string Name(900 * 1024, 'x');
		for (uint32 Index = 0; Index < 16; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			auto Request = Service.RequestCompute(Initializer, Name);
			if (!Request.IsAccepted())
			{
				EXPECT_EQ(Request.GetRejection(), ERHIPipelineRequestRejection::CapacityExceeded);
				break;
			}
			Requests.push_back(Request);
		}
		EXPECT_GT(Requests.size(), 0u);
		EXPECT_LT(Requests.size(), 16u);
		EXPECT_LE(Service.GetStatistics().UnfinishedDescriptionBytes, 16ull * 1024 * 1024);
		Release.set_value();
		for (const auto& Request : Requests) EXPECT_TRUE(Request.Wait());
		Service.CloseAndJoin();
		EXPECT_EQ(Service.GetStatistics().UnfinishedDescriptionBytes, 0u);
	}

	TEST_F(FRHIPipelineCreationTests, ForeignTaskScopesRejectBeforeCoreConstructionWithoutStarvingWorker)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		FRHIPipelineCreationService Service(Capabilities(), Backend([](const auto&, const auto&) {
			return MakeRefCount<FRHIComputePipelineState>();
		}));
		std::promise<FRHIPipelineCreationRequest> Submitted;
		auto SubmittedFuture = Submitted.get_future();
		auto Scope = CreateTaskScope();
		Tasks::FTaskGroup Group(Scope.GetToken());
		auto Producer = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
			auto Request = Service.RequestCompute(Initializer, "single worker caller");
			EXPECT_FALSE(Request.Wait());
			EXPECT_EQ(Request.GetRejection(), ERHIPipelineRequestRejection::Unsupported);
			Submitted.set_value(Request);
		});
		EXPECT_EQ(SubmittedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
		auto Request = SubmittedFuture.get();
		EXPECT_FALSE(Request.IsAccepted());
		Request = Service.RequestCompute(Initializer, "owning caller");
		EXPECT_TRUE(Request.Wait());
		auto Completion = Request.GetCompletion();
		Request = {};
		Service.CloseAndJoin();
		EXPECT_EQ(Service.GetStatistics().ActiveObservers, 0u);
		EXPECT_TRUE(Completion.IsReady());
		Group.Close();
		EXPECT_EQ(Scope.Wait(), ETaskScopeWaitResult::Quiescent);
	}

	TEST_F(FRHIPipelineCreationTests, MixedBatchPreservesPartialResultsAndRecoverableFailuresCanRetry)
	{
		auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		std::atomic<bool> Fail = true;
		FRHIPipelineCreationService Service(Capabilities(), Backend([&](const auto& Inputs, const auto&) {
			if (Inputs.DebugName == "fail" && Fail.exchange(false)) throw FRHIRecoverableCreationError("candidate failed");
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
		auto Batch = Service.RequestComputeBatch(Items);
		ASSERT_EQ(Batch.Items.size(), 3u);
		EXPECT_TRUE(Batch.Items[0].Wait());
		EXPECT_FALSE(Batch.Items[1].Wait());
		EXPECT_EQ(Batch.Items[1].GetResult().Diagnostic, "candidate failed");
		EXPECT_EQ(Batch.Items[2].GetRejection(), ERHIPipelineRequestRejection::InvalidDescription);
		EXPECT_TRUE(Service.RequestCompute(Items[1].Initializer, "retry").Wait());
		Items.resize(257);
		EXPECT_EQ(Service.RequestComputeBatch(Items).Rejection, ERHIPipelineRequestRejection::CapacityExceeded);
		EXPECT_EQ(Service.RequestCompute(Items[0].Initializer, std::string(1024 * 1024, 'x')).GetRejection(),
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
		FRHIPipelineCreationService Service(Capabilities(), std::move(Callbacks));
		auto Request = Service.RequestCompute(Initializer, "terminal");
		EXPECT_FALSE(Request.Wait());
		EXPECT_TRUE(Published.load());
		EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Failed);
		EXPECT_THROW(Request.GetResult(), std::logic_error);
		EXPECT_EQ(Service.RequestCompute(Initializer, "late").GetRejection(), ERHIPipelineRequestRejection::Closed);
		Service.CloseAndJoin();
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
			FRHIPipelineCreationService Service(Capabilities(), Backend([&](const auto&, const auto&) {
				Entered.set_value();
				Released.wait();
				Exited = true;
				return MakeRefCount<FRHIComputePipelineState>();
			}));
			Request = Service.RequestCompute(Initializer, "close in flight");
			EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			auto ReleaseAfterCancel = std::async(std::launch::async, [&] {
				EXPECT_FALSE(Request.Wait());
				EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Canceled);
				Release.set_value();
			});
			Service.CloseAndJoin();
			ReleaseAfterCancel.get();
			EXPECT_TRUE(Exited.load());
			EXPECT_EQ(Service.RequestCompute(Initializer, "closed").GetRejection(), ERHIPipelineRequestRejection::Closed);
		}
		EXPECT_EQ(Request.GetState(), ERHIPipelineRequestState::Canceled);
		EXPECT_TRUE(Request.GetCompletion().IsReady());
	}
}
