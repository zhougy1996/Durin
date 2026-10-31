#include "Shader/ShaderMapPreparation.h"
#include <gtest/gtest.h>

#include "RenderResourceCreation.h"
#include "ShaderPipelineCache.h"
#include "Shader/Shader.h"
#include "DynamicRHI.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/ThreadEvent.h"
#include "RHICommandList.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FResult = TRenderResourceCreateResult<int>;
		using FSlot = TRenderResourceCreationSlot<int>;

		class FPipelineSlotTestRHI final : public FDynamicRHI
		{
		public:
			FPipelineSlotTestRHI()
			{
				FRHICapabilities Caps;
				Caps.SupportedTextureDimensions = ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube;
				Caps.MaxTextureDimension2D = 4096;
				Caps.MaxTextureDimensionCube = 2048;
				Caps.MaxTextureArrayLayers = 256;
				Caps.ColorSampleCounts = ERHISampleCountFlags::Samples1;
				Caps.DepthSampleCounts = ERHISampleCountFlags::Samples1;
				Caps.MaxComputeWorkGroupCount = {65535, 65535, 65535};
				PublishCapabilities(Caps);
			}
			~FPipelineSlotTestRHI() override { RHIStopPipelineCreation(); RHIRetirePipelineCreationResults(); }
			std::function<void(uint32)> BeforeCreate;
			std::function<bool(uint32)> FailCreate;
			std::atomic<uint32> Creations = 0;
			auto Init(const FRHIInitializationContext&) -> void override {}
			auto Shutdown() -> void override {}
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHICreateViewport(const FRHIViewportCreateInfo&) -> FViewportRHIRef override { return {}; }
			auto RHIResizeViewport(FRHIViewport*, uint32, uint32, bool) -> void override {}
			auto RHICreateGraphicsPipelineState(FName, const FGraphicsPipelineStateInitializer&) -> FGraphicsPipelineStateRHIRef override { ADD_FAILURE(); return {}; }
			auto RHICreateComputePipelineState(FName, const FComputePipelineStateInitializer&) -> FComputePipelineStateRHIRef override { ADD_FAILURE(); return {}; }
			auto RHIGetDefaultContext() -> IRHICommandContext* override { return nullptr; }
			auto RHIGetViewportBackBuffer(FRHIViewport*) -> FTextureRHIRef override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&) -> FVertexDeclarationRHIRef override { return {}; }
			auto RHIIsTextureSupported(const FRHITextureCreateDesc&) const -> bool override { return false; }
			auto RHICreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc&) -> FTextureRHIRef override
			{
				return {};
			}
			auto RHICreateSampler(const FRHISamplerDesc&) -> FSamplerRHIRef override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) -> FShaderRHIRef override { return {}; }
			auto RHICreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc&) -> FBufferRHIRef override
			{
				return {};
			}
			auto RHIGetPipelineStateCache() -> FRHIPipelineStateCache* override
			{
				std::lock_guard Lock(DeviceState.Mutex);
				if (DeviceState.Closed || !IsTaskSchedulerRunning() || !RHIGetCapabilities()) return nullptr;
				if (!DeviceState.PipelineCache) DeviceState.PipelineCache = std::make_unique<FRHIPipelineStateCache>(*RHIGetCapabilities(), CreatePipelineCompileBackend());
				return DeviceState.PipelineCache.get();
			}
			void RHIStopPipelineCreation() override
			{
				FRHIPipelineStateCache* Cache;
				{ std::lock_guard Lock(DeviceState.Mutex); DeviceState.Closed = true; Cache = DeviceState.PipelineCache.get(); }
				if (Cache) Cache->StopAndWait();
			}
			void RHIRetirePipelineCreationResults() override
			{ if (DeviceState.PipelineCache) DeviceState.PipelineCache->ReleaseResources(); }
			bool RHIIsPipelineCreationClosed() const override
			{ std::lock_guard Lock(DeviceState.Mutex); return DeviceState.Closed || (DeviceState.PipelineCache && DeviceState.PipelineCache->IsClosed()); }
			auto RHIGetPipelineCreationStatistics() const -> FRHIPipelineCreationStatistics override
			{ std::lock_guard Lock(DeviceState.Mutex); return DeviceState.PipelineCache ? DeviceState.PipelineCache->GetStatistics() : FRHIPipelineCreationStatistics{}; }
		protected:
			auto CreatePipelineCompileBackend() -> FRHIPipelineCompileBackend
			{
				return {
					.FindGraphics = [](const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
					.FindCompute = [this](const auto& Key) -> FComputePipelineStateRHIRef {
						std::lock_guard Lock(Mutex);
						const auto It = Cache.find(Key);
						return It == Cache.end() ? nullptr : It->second;
					},
					.CreateGraphics = [](const auto&, const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
					.CreateCompute = [this](const auto&, const auto& Key) -> FComputePipelineStateRHIRef {
						++Creations;
						const auto Slot = Key.PipelineLayout->BindingLayouts[0].BindingLayouts[0].Slot;
						if (BeforeCreate) BeforeCreate(Slot);
						if (FailCreate && FailCreate(Slot)) return {};
						auto Result = MakeRefCount<FRHIComputePipelineState>();
						std::lock_guard Lock(Mutex);
						Cache.emplace(Key, Result);
						return Result;
					},
					.PublishTerminalFailure = [](std::exception_ptr) { ADD_FAILURE(); }
				};
			}
		private:
			std::mutex Mutex;
			std::unordered_map<FComputePipelineStateKey, FComputePipelineStateRHIRef, FComputePipelineStateKeyHasher> Cache;
			struct FTestDeviceState
			{
				mutable std::mutex Mutex;
				std::unique_ptr<FRHIPipelineStateCache> PipelineCache;
				bool Closed = false;
			} DeviceState;
		};

		auto MakeError(
			EDependency RetryDependencies = EDependency::Shader,
			ERenderResourceCreateErrorReason Reason =
				ERenderResourceCreateErrorReason::Unspecified)
			-> FRenderResourceCreateError
		{
			return {
				.Category = ERenderResourceCreateErrorCategory::ShaderCompile,
				.Reason = Reason,
				.Context = "TestResource",
				.Identity = "variant=1",
				.Cause = std::string("Shader entry-point request is invalid."),
				.RetryDependencies = RetryDependencies,
			};
		}

		TEST(FRenderResourceCreationTests, UnpreparedShadersDeferOnlyTheirAttemptAndRetryWithoutGenerationChange)
		{
			FShaderType Type("PendingSlotShader", "/Unit/PendingSlot", EShaderFrequency::Vertex, "VertexMain");
			const FShaderType* Types[] = {&Type};
			FSlot Slot;
			FRenderResourceGeneration Generation;
			uint32 Diagnostics = 0;
			auto Report = [&](const FRenderResourceCreateDiagnostic&) { ++Diagnostics; };
			{
				FPreparedShaderMapScope Scope;
				EXPECT_EQ(Slot.Resolve(Generation, [&] {
					FShaderMapBase Map;
					const auto Completion = InitializeShaderMapFromTypes(Map, Types, {});
					EXPECT_FALSE(Completion);
					EXPECT_EQ(Completion.Status, EShaderBuildStatus::Pending);
					EXPECT_TRUE(Completion.Error.empty());
					if (Completion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
					return FResult::Failure(MakeError());
				}, Report), nullptr);
				EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Pending);
				EXPECT_FALSE(Slot.GetFailure());
				EXPECT_EQ(Diagnostics, 0u);
				// An earlier pending shader must not hide an independent terminal error.
				FSlot FailedSlot;
				EXPECT_EQ(FailedSlot.Resolve(Generation, [] { return FResult::Failure(MakeError()); }, Report), nullptr);
				EXPECT_EQ(FailedSlot.GetAvailability(), ERenderResourceAvailability::Failed);
				EXPECT_EQ(Diagnostics, 1u);
			}
			FPreparedShaderMapScope NextCapture;
			const auto* Product = Slot.Resolve(Generation, [] { return FResult::Success(42); }, Report);
			ASSERT_NE(Product, nullptr);
			EXPECT_EQ(*Product, 42);
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Ready);
			EXPECT_EQ(Diagnostics, 1u);
		}

		TEST(FRenderResourceCreationTests, ExplicitPendingPropagatesThroughNestedSlotsWithoutHidingSiblingFailure)
		{
			FSlot Parent, Child, FailedSibling;
			uint32 Diagnostics = 0;
			auto Report = [&](const auto&) { ++Diagnostics; };
			{
				FRenderPipelinePreparationBatch Batch;
				EXPECT_EQ(Parent.Resolve({}, [&] {
					EXPECT_EQ(Child.Resolve({}, [] { return FResult::Pending(); }, Report), nullptr);
					return FResult::Failure(MakeError());
				}, Report), nullptr);
				EXPECT_EQ(Parent.GetAvailability(), ERenderResourceAvailability::Pending);
				EXPECT_FALSE(Parent.GetFailure());
				EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
				EXPECT_EQ(Batch.GetRequestCount(), 0u);
				EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::WaitUnavailable);
				EXPECT_EQ(Diagnostics, 0u);
				EXPECT_EQ(FailedSibling.Resolve({}, [] { return FResult::Failure(MakeError()); }, Report), nullptr);
				EXPECT_EQ(FailedSibling.GetAvailability(), ERenderResourceAvailability::Failed);
				EXPECT_EQ(Diagnostics, 1u);
			}
			FRenderPipelinePreparationBatch NextBatch;
			EXPECT_FALSE(FRenderPipelinePreparationBatch::HasPending());
			EXPECT_NE(Parent.Resolve({}, [] { return FResult::Success(42); }, Report), nullptr);
			EXPECT_EQ(Parent.GetAvailability(), ERenderResourceAvailability::Ready);
			EXPECT_EQ(Diagnostics, 1u);
		}

		TEST(FRenderResourceCreationTests, PreparationLeasesAreReleasedOnPublicationFailureCancellationResetAndGenerationChange)
		{
			FSlot Slot;
			std::weak_ptr<const int> Lifetime;
			auto Report = [](const auto&) {};
			auto Pend = [&] {
				Slot.Reset();
				auto Lease = std::make_shared<const int>(42);
				Lifetime = Lease;
				EXPECT_EQ(Slot.Resolve({}, [&] {
					FRenderPipelineRequestScope::RetainPreparation(Lease);
					return FResult::Pending();
				}, Report), nullptr);
				Lease.reset();
				EXPECT_FALSE(Lifetime.expired());
			};
			Pend();
			EXPECT_NE(Slot.Resolve({}, [] { return FResult::Success(42); }, Report), nullptr);
			EXPECT_TRUE(Lifetime.expired());
			Pend();
			EXPECT_EQ(Slot.Resolve({}, [] { return FResult::Failure(MakeError()); }, Report), nullptr);
			EXPECT_TRUE(Lifetime.expired());
			Pend();
			EXPECT_EQ(Slot.Resolve({}, [] { return FResult::Canceled(); }, Report), nullptr);
			EXPECT_TRUE(Lifetime.expired());
			Pend();
			Slot.Reset();
			EXPECT_TRUE(Lifetime.expired());
			Pend();
			FRenderResourceGeneration Reloaded;
			++Reloaded.Shader;
			EXPECT_EQ(Slot.Resolve(Reloaded, [&] {
				EXPECT_TRUE(Lifetime.expired());
				return FResult::Pending();
			}, Report), nullptr);
		}

		TEST(FRenderResourceCreationTests, CancellationDoesNotReportFailureOrSuppressRetry)
		{
			FSlot Slot;
			FRenderResourceGeneration Generation;
			uint32 Diagnostics = 0;
			auto Report = [&](const FRenderResourceCreateDiagnostic&) { ++Diagnostics; };
			EXPECT_EQ(Slot.Resolve(Generation, [] { return FResult::Canceled(); }, Report), nullptr);
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Canceled);
			EXPECT_EQ(Diagnostics, 0u);
			const auto* Product = Slot.Resolve(Generation, [] { return FResult::Success(42); }, Report);
			ASSERT_NE(Product, nullptr);
			EXPECT_EQ(*Product, 42);
			EXPECT_EQ(Diagnostics, 0u);
		}

		TEST(FRenderResourceCreationTests, FingerprintsUseResourceIdentityAndIgnoreDiagnosticWording)
		{
			auto First = MakeError(); First.Cause = std::string("first wording");
			auto Second = First; Second.Cause = std::string("different wording"); Second.AttemptedGeneration.Shader = 99;
			EXPECT_EQ(First.GetFingerprint(), Second.GetFingerprint());
			Second.Identity = "/Other/Source"; EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
			Second = First; Second.Reason = ERenderResourceCreateErrorReason::PipelineCreationFailed;
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
		}

		TEST(FRenderResourceCreationTests, StoredFailureOwnsCauseAfterProducerDestruction)
		{
			FSlot Slot; std::optional<FRenderResourceCreateDiagnostic> Report;
			Slot.Resolve({}, [] {
				auto Error = MakeError(); Error.Cause = std::string("Cannot read /Temporary/Source: permission denied");
				return FResult::Failure(std::move(Error));
			}, [&](auto Diagnostic) { Report = std::move(Diagnostic); });
			Slot.Reset(); ASSERT_TRUE(Report && Report->Error);
			EXPECT_EQ(FormatRenderResourceCreateError(*Report->Error), "Cannot read /Temporary/Source: permission denied");
		}

		TEST(
			FRenderResourceCreationTests,
			StructuredReasonParticipatesInFailureIdentity)
		{
			const FRenderResourceCreateError Generic = MakeError();
			const FRenderResourceCreateError GlobalShaderUnavailable = MakeError(
				EDependency::Shader,
				ERenderResourceCreateErrorReason::GlobalShaderUnavailable);
			EXPECT_NE(Generic.GetFingerprint(),
				GlobalShaderUnavailable.GetFingerprint());
		}

		TEST(FRenderResourceCreationTests, PreparationBatchReportsCapacityWithoutWaitingAndKeepsNestedStateIndependent)
		{
			class FUnsubmittedPipeline final : public FPipelineState
			{
			public:
				FUnsubmittedPipeline() : FPipelineState({}, {}) {}
			};
			FRenderPipelinePreparationBatch Batch;
			EXPECT_FALSE(Batch.HasCapacityExceeded());
			std::shared_ptr<FPipelineState> Last;
			for (size_t Index = 0; Index < FRenderPipelinePreparationBatch::MaximumRequests; ++Index)
			{
				Last = std::make_shared<FUnsubmittedPipeline>();
				FRenderPipelinePreparationBatch::Add(Last);
			}
			EXPECT_FALSE(Batch.HasCapacityExceeded());
			FRenderPipelinePreparationBatch::Add(Last);
			EXPECT_FALSE(Batch.HasCapacityExceeded());
			EXPECT_EQ(Batch.GetRequestCount(), FRenderPipelinePreparationBatch::MaximumRequests);
			FRenderPipelinePreparationBatch::Add(std::make_shared<FUnsubmittedPipeline>());
			EXPECT_TRUE(Batch.HasCapacityExceeded());
			EXPECT_EQ(Batch.GetRequestCount(), FRenderPipelinePreparationBatch::MaximumRequests);
			{
				FRenderPipelinePreparationBatch Nested;
				EXPECT_FALSE(Nested.HasCapacityExceeded());
				EXPECT_EQ(Nested.GetRequestCount(), 0u);
			}
			EXPECT_TRUE(Batch.HasCapacityExceeded());
			EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::CapacityExceeded);
		}

		TEST(FRenderResourceCreationTests, PreparationBatchReportsTerminalResultsAndJoinsAllRequests)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			RHI.FailCreate = [](uint32 Slot) { return Slot == 0; };
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
			auto Failed = PipelineStateCache::PrecacheComputePipelineState(Initializer, "failed");
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 1;
			auto Ready = PipelineStateCache::PrecacheComputePipelineState(Initializer, "ready");
			ASSERT_TRUE(Failed);
			ASSERT_TRUE(Ready);
			{
				FRenderPipelinePreparationBatch Batch;
				EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::Empty);
				FRenderPipelinePreparationBatch::Add(*Failed);
				FRenderPipelinePreparationBatch::Add(*Ready);
				FRenderPipelinePreparationBatch::Add(*Ready);
				EXPECT_EQ(Batch.GetRequestCount(), 2u);
				EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::Failed);
				EXPECT_EQ((*Failed)->GetState(), ERHIPipelineRequestState::Failed);
				EXPECT_EQ((*Ready)->GetState(), ERHIPipelineRequestState::Ready);
				{
					FRenderPipelinePreparationBatch Nested;
					EXPECT_FALSE(FRenderPipelinePreparationBatch::HasPending());
					FRenderPipelinePreparationBatch::Add(*Ready);
					EXPECT_EQ(Nested.Wait(), ERenderPipelinePreparationWait::Ready);
				}
				EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
			}
			EXPECT_FALSE(FRenderPipelinePreparationBatch::HasPending());
			EXPECT_EQ(RHI.Creations, 2u);
		}

		TEST(FRenderResourceCreationTests, PipelineRequestsNeverFallBackToDirectCreation)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			ASSERT_FALSE(IsTaskSchedulerRunning());
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			struct FReset { ~FReset() { GDynamicRHI = nullptr; RHIFlushDeferredResources(); } } Reset;
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Compute;
			Compute.ComputeShader = Shader;
			EXPECT_FALSE(FRenderPipelineRequestScope::Compute("no scope", Compute));
			FRenderPipelineRequests Requests;
			FRenderPipelineRequestScope Scope(Requests, {});
			EXPECT_FALSE(FRenderPipelineRequestScope::Compute("no scheduler", Compute));
			EXPECT_FALSE(FRenderPipelineRequestScope::Graphics("no scheduler", {}));
			EXPECT_EQ(RHI.Creations, 0u);
		}

		TEST(FRenderResourceCreationTests, AsyncPipelineFailureHasNoBackendCause)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			RHI.FailCreate = [](uint32) { return true; };
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
			FSlot Slot;
			auto Resolve = [&] {
				return Slot.Resolve({}, [&] {
					auto Pipeline = FRenderPipelineRequestScope::Compute("native failure", Initializer);
					if (Pipeline) return FResult::Success(1);
					auto Error = MakeError();
					Error.Category = ERenderResourceCreateErrorCategory::GraphicsPipeline;
					Error.Cause.clear();
					Error.Reason = ERenderResourceCreateErrorReason::PipelineCreationFailed;
					return FResult::Failure(std::move(Error));
				}, [](const auto&) {});
			};
			{
				FRenderPipelinePreparationBatch Batch;
				EXPECT_EQ(Resolve(), nullptr);
				Batch.Wait();
			}
			EXPECT_EQ(Resolve(), nullptr);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			EXPECT_TRUE(Slot.GetFailure()->Cause.empty());
			EXPECT_EQ(Slot.GetFailure()->Reason, ERenderResourceCreateErrorReason::PipelineCreationFailed);
			Slot.Reset();
		}

		TEST(FRenderResourceCreationTests, PendingPipelinesRetryAndLateGenerationsCannotReplaceCurrentPayload)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			FThreadEvent Entered, Release, RefreshEntered, RefreshRelease;
			RHI.BeforeCreate = [&](uint32 Variant) {
				if (Variant == 0) { Entered.Trigger(); Release.Wait(); }
				if (Variant == 2) { RefreshEntered.Trigger(); RefreshRelease.Wait(); }
			};
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
			using FPipelineResult = TRenderResourceCreateResult<FComputePipelineStateRHIRef>;
			TRenderResourceCreationSlot<FComputePipelineStateRHIRef> Slot(EDependency::Shader | EDependency::Device | EDependency::Manual);
			FRenderResourceGeneration Generation;
			uint32 Diagnostics = 0;
			const auto Resolve = [&] {
				return Slot.Resolve(Generation, [&] {
					auto Result = FRenderPipelineRequestScope::Compute("same diagnostic name", Initializer);
					return Result ? FPipelineResult::Success(Result) : FPipelineResult::Failure(MakeError());
				}, [&](const auto&) { ++Diagnostics; });
			};
			EXPECT_EQ(Resolve(), nullptr);
			EXPECT_TRUE(Entered.WaitFor(5.0));
			EXPECT_EQ(Resolve(), nullptr);
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Pending);
			EXPECT_EQ(RHI.RHIGetPipelineCreationStatistics().ActiveObservers, 1u);
			++Generation.Shader;
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 1;
			EXPECT_EQ(Resolve(), nullptr);
			Release.Trigger();
			auto Current = PipelineStateCache::PrecacheComputePipelineState(Initializer, "wait current");
			ASSERT_TRUE(Current);
			EXPECT_TRUE((*Current)->Wait());
			ASSERT_NE(Resolve(), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), (*Current)->GetRHIPipeline());
			EXPECT_EQ(RHI.Creations.load(), 2u);
			const auto Old = *Slot.GetPayload();
			FRenderPipelinePreparationBatch RefreshBatch;
			++Generation.Manual;
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 2;
			EXPECT_EQ(Resolve(), nullptr);
			EXPECT_EQ(Slot.GetPayload(), nullptr);
			EXPECT_TRUE(RefreshEntered.WaitFor(5.0));
			EXPECT_EQ(RefreshBatch.GetRequestCount(), 1u);
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Pending);
			++Generation.Device;
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 3;
			EXPECT_EQ(Resolve(), nullptr);
			RefreshRelease.Trigger();
			EXPECT_EQ(RefreshBatch.Wait(), ERenderPipelinePreparationWait::Ready);
			auto Latest = PipelineStateCache::PrecacheComputePipelineState(Initializer, "wait latest");
			ASSERT_TRUE(Latest);
			EXPECT_TRUE((*Latest)->Wait());
			ASSERT_NE(Resolve(), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), (*Latest)->GetRHIPipeline());
			EXPECT_NE(*Slot.GetPayload(), Old);
			EXPECT_EQ(Diagnostics, 0u);
			Slot.Reset();
		}

		TEST(FRenderResourceCreationTests, NativeFailureRetriesNextPreparationAndDeduplicatesDiagnostics)
		{
			FSlot Slot(EDependency::Device);
			int Attempts = 0;
			std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
			auto Factory = [&]() {
				if (++Attempts == 3) return FResult::Success(42);
				return FResult::Failure({
					.Category = ERenderResourceCreateErrorCategory::RHIResource,
					.Reason = ERenderResourceCreateErrorReason::ResourceCreationFailed,
					.Context = "native resource",
					.Identity = "candidate",
					.RetryDependencies = EDependency::Device,
				});
			};
			auto Reporter = [&](const auto& Diagnostic) { Diagnostics.push_back(Diagnostic); };
			EXPECT_EQ(Slot.Resolve({}, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.Resolve({}, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 2);
			ASSERT_EQ(Diagnostics.size(), 1u);
			ASSERT_NE(Slot.Resolve({}, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 42);
			ASSERT_EQ(Diagnostics.size(), 2u);
			EXPECT_EQ(Diagnostics.back().Kind, ERenderResourceCreateDiagnosticKind::Recovery);
			EXPECT_NE(Slot.Resolve({}, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 3);
		}

		TEST(
			FRenderResourceCreationTests,
			InitialFailureIsSuppressedUntilRelevantGenerationChanges)
		{
			FSlot Slot(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
			auto Factory = [&]() {
				++Attempts;
				return Attempts == 1
					? FResult::Failure(MakeError())
					: FResult::Success(42);
			};
			auto Reporter = [&](FRenderResourceCreateDiagnostic Diagnostic) {
				Diagnostics.push_back(std::move(Diagnostic));
			};

			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 1);
			EXPECT_EQ(Diagnostics.size(), 1);

			++Generation.Manual;
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 1);

			++Generation.Shader;
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 42);
			EXPECT_EQ(Attempts, 2);
			ASSERT_EQ(Diagnostics.size(), 2);
			EXPECT_EQ(
				Diagnostics.back().Kind,
				ERenderResourceCreateDiagnosticKind::Recovery);
			ASSERT_TRUE(Diagnostics.back().Error.has_value());
			EXPECT_EQ(Diagnostics.back().Error->Context, "TestResource");
			EXPECT_EQ(Diagnostics.back().Error->Identity, "variant=1");
		}

		TEST(
			FRenderResourceCreationTests,
			LateFailureDoesNotPublishPartialCandidate)
		{
			struct FCandidate
			{
				int FirstStep = 0;
				int SecondStep = 0;
			};
			using FCandidateResult = TRenderResourceCreateResult<FCandidate>;
			TRenderResourceCreationSlot<FCandidate> Slot(EDependency::Device);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};

			FCandidate Candidate{.FirstStep = 7};
			EXPECT_EQ(
				Slot.Resolve(
					Generation,
					[&]() {
						return FCandidateResult::Failure(
							MakeError(EDependency::Device));
					},
					Reporter),
				nullptr);
			EXPECT_EQ(Slot.GetPayload(), nullptr);
		}

		TEST(
			FRenderResourceCreationTests,
			FailedShaderRefreshExposesNoPayload)
		{
			FSlot Slot(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() {
				++Attempts;
				if (Attempts == 2)
				{
					EXPECT_EQ(
						Slot.GetAvailability(),
						ERenderResourceAvailability::Pending);
					EXPECT_EQ(Slot.GetPayload(), nullptr);
				}
				return Attempts == 1
					? FResult::Success(11)
					: FResult::Failure(MakeError());
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			++Generation.Shader;
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.GetPayload(), nullptr);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Failed);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			EXPECT_EQ(Attempts, 2);
		}

		TEST(
			FRenderResourceCreationTests,
			SuccessfulRefreshAtomicallyReplacesPayload)
		{
			FSlot Slot(EDependency::Shader);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() {
				++Attempts;
				if (Attempts == 2)
				{
					EXPECT_EQ(
						Slot.GetAvailability(),
						ERenderResourceAvailability::Pending);
					EXPECT_EQ(Slot.GetPayload(), nullptr);
				}
				return FResult::Success(Attempts == 1 ? 4 : 8);
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 4);
			Generation.Advance(EDependency::Shader);
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 8);
			EXPECT_EQ(
				Slot.GetPayloadGeneration().Shader,
				Generation.Shader);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Ready);
		}

		TEST(
			FRenderResourceCreationTests,
			DeviceGenerationClearsPayloadBeforeRetry)
		{
			FSlot Slot(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() {
				++Attempts;
				return Attempts == 1
					? FResult::Success(9)
					: FResult::Failure(MakeError(EDependency::Device));
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			++Generation.Device;
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.GetPayload(), nullptr);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Failed);
			ASSERT_NE(Slot.GetFailure(), nullptr);
		}

		TEST(
			FRenderResourceCreationTests,
			ReentrantResolveDoesNotInvokeFactoryTwiceOrExposePartialPayload)
		{
			FSlot Slot(EDependency::Shader);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() -> FResult {
				++Attempts;
				EXPECT_EQ(
					Slot.GetAvailability(),
					ERenderResourceAvailability::Pending);
				EXPECT_EQ(Slot.GetPayload(), nullptr);
				EXPECT_EQ(
					Slot.Resolve(
						Generation,
						[]() { return FResult::Success(99); },
						Reporter),
					nullptr);
				return FResult::Success(5);
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 1);
			EXPECT_EQ(*Slot.GetPayload(), 5);
		}

		TEST(
			FRenderResourceCreationTests,
			ReentrantRefreshExposesNoPayload)
		{
			FSlot Slot(EDependency::Shader);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			ASSERT_NE(
				Slot.Resolve(
					Generation,
					[]() { return FResult::Success(6); },
					Reporter),
				nullptr);
			Generation.Advance(EDependency::Shader);
			int NestedFactoryCalls = 0;

			ASSERT_NE(
				Slot.Resolve(
					Generation,
					[&]() {
						EXPECT_EQ(
							Slot.GetAvailability(),
							ERenderResourceAvailability::Pending);
						int* Fallback = Slot.Resolve(
							Generation,
							[&]() {
								++NestedFactoryCalls;
								return FResult::Success(99);
							},
							Reporter);
						EXPECT_EQ(Fallback, nullptr);
						return FResult::Success(7);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(NestedFactoryCalls, 0);
			EXPECT_EQ(*Slot.GetPayload(), 7);
		}

		TEST(
			FRenderResourceCreationTests,
			RepeatedFailureReportsOnceAndRecoveryReportsOnce)
		{
			FSlot Slot(EDependency::Manual);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
			auto Factory = [&]() {
				++Attempts;
				return Attempts == 1
					? FResult::Failure(MakeError(EDependency::Manual))
					: FResult::Success(3);
			};
			auto Reporter = [&](FRenderResourceCreateDiagnostic Diagnostic) {
				Diagnostics.push_back(std::move(Diagnostic));
			};

			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			ASSERT_EQ(Diagnostics.size(), 1);
			++Generation.Manual;
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			ASSERT_EQ(Diagnostics.size(), 2);
			EXPECT_EQ(
				Diagnostics.back().Kind,
				ERenderResourceCreateDiagnosticKind::Recovery);
		}

		TEST(
			FRenderResourceCreationTests,
			FailureFingerprintTracksOwnedDiagnosticAndResetClearsState)
		{
			FSlot Slot(EDependency::Manual);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			const FRenderResourceCreateError Error =
				MakeError(EDependency::Manual);

			EXPECT_EQ(
				Slot.Resolve(
					Generation,
					[&]() { return FResult::Failure(Error); },
					Reporter),
				nullptr);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			ASSERT_TRUE(Slot.GetFailureFingerprint().has_value());
			EXPECT_EQ(
				*Slot.GetFailureFingerprint(),
				Slot.GetFailure()->GetFingerprint());
			EXPECT_EQ(
				Slot.GetAttemptedGeneration(),
				Generation);

			Slot.Reset();
			EXPECT_EQ(Slot.GetPayload(), nullptr);
			EXPECT_EQ(Slot.GetFailure(), nullptr);
			EXPECT_FALSE(Slot.GetFailureFingerprint().has_value());
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Uninitialized);
		}
		TEST(FRenderResourceCreationTests, LoadingPreparationBudgetsRetainsCachedPayloadsAndCancelsIndependently)
		{
			class FPreparationTask final : public FRunnable
			{
			public:
				auto Run() -> uint32 override
				{
					auto Owner = FRenderResourcePreparation::Create();
					auto Other = FRenderResourcePreparation::Create();
					auto Resource = std::make_shared<int>(7);
					std::weak_ptr<int> Weak = Resource;
					{
						FRenderResourcePreparationScope Scope(Owner);
						FRenderResourcePreparationScope::Retain(Resource, 4);
						FRenderResourcePreparationScope::Retain(Resource, 8);
						EXPECT_EQ(Owner.GetProgress().AccountedBytes, 8u);
						uint32 Admitted = 0;
						try { for (; Admitted < 100; ++Admitted) FRenderResourcePreparationScope::BeforeShader(); }
						catch (const FRenderResourcePreparationYield&) {}
						EXPECT_GE(Admitted, 1u); EXPECT_LE(Admitted, 8u);
						Scope.Complete(ERenderResourcePreparationState::Pending);
					}
					{
						FRenderResourcePreparationScope Scope(Other);
						FRenderResourcePreparationScope::Retain(Resource, 4);
						Scope.Complete(ERenderResourcePreparationState::Ready);
					}
					{
						FRenderResourcePreparationUseScope Use(Other);
						FRenderResourcePreparationScope::BeforeShader();
						FRenderResourcePreparationScope::BeforePipeline();
					}
					EXPECT_EQ(Other.GetProgress().UncoveredShaderAdmissions, 1u);
					EXPECT_EQ(Other.GetProgress().UncoveredPipelineAdmissions, 1u);
					EXPECT_EQ(Other.GetProgress().State, ERenderResourcePreparationState::Ready);
					Resource.reset();
					Owner.RequestCancel();
					Owner.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
					EXPECT_FALSE(Weak.expired());
					EXPECT_EQ(Other.GetProgress().State, ERenderResourcePreparationState::Ready);
					Other.Retire_RenderThread(ERenderResourcePreparationState::Stale);
					EXPECT_TRUE(Weak.expired());

					FSlot Slot;
					Slot.Resolve({}, [] { return FResult::Success(42); }, [](const auto&) {});
					auto Cached = FRenderResourcePreparation::Create();
					{
						FRenderResourcePreparationScope Scope(Cached);
						EXPECT_EQ(*Slot.GetPayload(), 42);
						Scope.Complete(ERenderResourcePreparationState::Ready);
					}
					Slot.Reset();
					EXPECT_EQ(Cached.GetProgress().RetainedObjects, 1u);
					{
						FRenderResourcePreparationScope Scope(Cached);
						EXPECT_EQ(Cached.GetProgress().RetainedObjects, 0u);
						EXPECT_THROW(FRenderResourcePreparationScope::Retain(std::make_shared<int>(),
							257ull * 1024 * 1024), FRenderResourcePreparationCapacityExceeded);
					}
					auto Reloaded = FRenderResourcePreparation::Create();
					{
						FRenderResourcePreparationScope Scope(Reloaded);
						Scope.Complete(ERenderResourcePreparationState::Ready);
					}
					auto Failed = FRenderResourcePreparation::Create();
					std::weak_ptr<int> FailedResource;
					{
						FRenderResourcePreparationScope Scope(Failed);
						auto Resource = std::make_shared<int>(42);
						FailedResource = Resource;
						FRenderResourcePreparationScope::Retain(std::move(Resource), 4);
						Scope.Complete(ERenderResourcePreparationState::Failed);
					}
					EXPECT_TRUE(FailedResource.expired());
					EXPECT_EQ(Failed.GetProgress().RetainedObjects, 0u);
					Failed.RequestCancel();
					Failed.Retire_RenderThread(ERenderResourcePreparationState::Stale);
					EXPECT_EQ(Failed.GetProgress().State, ERenderResourcePreparationState::Failed);
					AdvanceShaderReloadGeneration();
					EXPECT_EQ(Reloaded.GetProgress().State, ERenderResourcePreparationState::Stale);
					Cached.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
					EXPECT_EQ(Cached.GetProgress().RetainedObjects, 0u);
					return 0;
				}
			} Task;
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Task, "PreparationContract", 0,
				EThreadPriority::Normal, EThreadRole::RenderingThread));
			ASSERT_NE(Thread, nullptr);
			Thread->WaitForCompletion();
		}

		TEST(FRenderResourceCreationTests, RecordingObservesUncoveredDemandWithoutConsumingPreparationBudget)
		{
			class FRecordingTask final : public FRunnable
			{
			public:
				auto Run() -> uint32 override
				{
					auto Owner = FRenderResourcePreparation::Create();
					auto Resource = std::make_shared<int>(42);
					{
						FRenderResourcePreparationScope Scope(Owner);
						FRenderResourcePreparationScope::BeforeShader();
						FRenderResourcePreparationScope::Retain(Resource, sizeof(int));
						{
							FRenderResourcePreparationUseScope Recording(Scope);
							EXPECT_FALSE(FRenderResourcePreparationScope::IsActive());
							// Exceed both count limits without depending on wall-clock timing.
							EXPECT_NO_THROW(for (uint32 Index = 0; Index < 32; ++Index) {
								FRenderResourcePreparationScope::BeforeShader();
								FRenderResourcePreparationScope::BeforePipeline();
							});
							// Recording must not extend the loading request's retained resource set.
							EXPECT_NO_THROW(FRenderResourcePreparationScope::Retain(Resource, 257ull * 1024 * 1024));
						}
						EXPECT_TRUE(FRenderResourcePreparationScope::IsActive());
						try
						{
							FRenderResourcePreparationUseScope Recording(Scope);
							throw 42;
						}
						catch (int) {}
						EXPECT_TRUE(FRenderResourcePreparationScope::IsActive());
						Scope.Complete(ERenderResourcePreparationState::Ready);
					}
					EXPECT_FALSE(FRenderResourcePreparationScope::IsActive());
					const auto Progress = Owner.GetProgress();
					EXPECT_EQ(Progress.State, ERenderResourcePreparationState::Ready);
					EXPECT_EQ(Progress.ShaderAdmissions, 1u);
					EXPECT_EQ(Progress.PipelineAdmissions, 0u);
					EXPECT_EQ(Progress.UncoveredShaderAdmissions, 32u);
					EXPECT_EQ(Progress.UncoveredPipelineAdmissions, 32u);
					EXPECT_EQ(Progress.RetainedObjects, 1u);
					EXPECT_EQ(Progress.AccountedBytes, sizeof(int));
					Owner.Retire_RenderThread(ERenderResourcePreparationState::Stale);
					return 0;
				}
			} Task;
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Task, "RecordingPreparationContract", 0,
				EThreadPriority::Normal, EThreadRole::RenderingThread));
			ASSERT_NE(Thread, nullptr);
			Thread->WaitForCompletion();
		}

		TEST(FRenderResourceCreationTests, PreparedPipelineRequestsSurviveSlotEviction)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FStop { ~FStop() { ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } Stop;
			struct FReset { ~FReset() { GDynamicRHI = nullptr; } } Reset;
			FPipelineSlotTestRHI RHI; GDynamicRHI = &RHI;
			class FPreparationTask final : public FRunnable
			{
			public:
				explicit FPreparationTask(FPipelineSlotTestRHI& InRHI) : RHI(InRHI) {}
				FPipelineSlotTestRHI& RHI;
				auto Run() -> uint32 override
				{
					auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
					FComputePipelineStateInitializer Initializer;
					Initializer.ComputeShader = Shader;
					Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
						EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
					auto Factory = [&] {
						auto Pipeline = FRenderPipelineRequestScope::Compute("retained", Initializer);
						return Pipeline ? FResult::Success(42) : FResult::Failure(MakeError());
					};
					FSlot PreparedSlot;
					auto Preparation = FRenderResourcePreparation::Create();
					const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
					while (!PreparedSlot.GetPayload() && std::chrono::steady_clock::now() < Deadline)
					{
						FRenderResourcePreparationScope Scope(Preparation);
						FRenderPipelinePreparationBatch Batch;
						PreparedSlot.Resolve({}, Factory, [](const auto&) {});
						Scope.Complete(PreparedSlot.GetPayload() ? ERenderResourcePreparationState::Ready : ERenderResourcePreparationState::Pending);
						std::this_thread::yield();
					}
					EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Ready);
					EXPECT_EQ(RHI.Creations.load(), 1u);
					PreparedSlot.Reset();
					FSlot ActivatedSlot;
					const auto* Activated = ActivatedSlot.Resolve({}, Factory, [](const auto&) {});
					EXPECT_NE(Activated, nullptr);
					EXPECT_EQ(RHI.Creations.load(), 1u);
					Preparation.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
					EXPECT_NE(ActivatedSlot.GetPayload(), nullptr);
					return 0;
				}
			} Task(RHI);
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Task, "PreparationEviction", 0,
				EThreadPriority::Normal, EThreadRole::RenderingThread));
			ASSERT_NE(Thread, nullptr); Thread->WaitForCompletion();
		}

		TEST(FRenderResourceCreationTests, BatchModeChangesApplyAtFrameBoundariesAndKeepCustomBudgets)
		{
			const auto Previous = FShaderPipelineCache::GetBatchMode();
			struct FRestore { FShaderPipelineCache::BatchMode Mode; ~FRestore() { FShaderPipelineCache::SetBatchMode(Mode); } } Restore{Previous};
			FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Background);
			FRenderResourceAdmissionBudget Budget;
			{
				FRenderResourceAdmissionScope Frame(Budget, 1);
				for (uint32 Index = 0; Index < 8; ++Index) EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
				EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforeShader());
			}
			FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Fast);
			{
				FRenderResourceAdmissionScope SameFrame(Budget, 1);
				EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforeShader());
			}
			{
				FRenderResourceAdmissionScope NextFrame(Budget, 2);
				for (uint32 Index = 0; Index < 32; ++Index) EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
				EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforeShader());
			}
			FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Background);
			{
				FRenderResourceAdmissionScope NextFrame(Budget, 3);
				for (uint32 Index = 0; Index < 8; ++Index) EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
				EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforeShader());
			}
			FRenderResourceAdmissionBudget Custom(1, 0, std::chrono::seconds(30));
			FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Fast);
			FRenderResourceAdmissionScope CustomFrame(Custom, 4);
			EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
			EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforeShader());
			EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforePipeline());
		}

		TEST(FRenderResourceCreationTests, PreparationAndDrawsShareOneAllowanceAcrossNestedScopes)
		{
			class FPreparationTask final : public FRunnable
			{
				auto Run() -> uint32 override
				{
					FRenderResourceAdmissionBudget Budget(1, 1, std::chrono::seconds(30));
					auto Owner = FRenderResourcePreparation::Create();
					auto Other = FRenderResourcePreparation::Create();
					{
						FRenderResourceAdmissionScope Frame(Budget, 1);
						{
							FRenderResourcePreparationScope Preparation(Owner);
							EXPECT_NO_THROW(FRenderResourcePreparationScope::BeforeShader());
							{
								FRenderResourcePreparationScope Nested(Other);
								EXPECT_THROW(FRenderResourcePreparationScope::BeforeShader(), FRenderResourcePreparationYield);
								EXPECT_NO_THROW(FRenderResourcePreparationScope::BeforePipeline());
							}
							EXPECT_THROW(FRenderResourcePreparationScope::BeforePipeline(), FRenderResourcePreparationYield);
						}
						EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforeShader());
						EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforePipeline());
					}
					{
						FRenderResourceAdmissionScope NextFrame(Budget, 2);
						EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
						FRenderResourcePreparationScope Preparation(Owner);
						EXPECT_THROW(FRenderResourcePreparationScope::BeforeShader(), FRenderResourcePreparationYield);
						EXPECT_NO_THROW(FRenderResourcePreparationScope::BeforePipeline());
					}
					EXPECT_EQ(Owner.GetProgress().ShaderAdmissions, 1u);
					EXPECT_EQ(Owner.GetProgress().PipelineAdmissions, 1u);
					EXPECT_EQ(Other.GetProgress().PipelineAdmissions, 1u);
					return 0;
				}
			} Task;
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Task, "SharedPreparationBudget", 0,
				EThreadPriority::Normal, EThreadRole::RenderingThread));
			ASSERT_NE(Thread, nullptr);
			Thread->WaitForCompletion();
		}

		TEST(FRenderResourceCreationTests, DeferredSinglePipelineMarksItsEnclosingBatchPending)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
			FRenderResourceAdmissionBudget Budget(0, 0, std::chrono::seconds(30));
			FRenderResourceAdmissionScope Admission(Budget, 1);
			FRenderPipelinePreparationBatch Batch;
			FSlot Slot;
			EXPECT_EQ(Slot.Resolve({}, [&] {
				const auto Pipeline = FRenderPipelineRequestScope::Compute("DeferredSinglePipeline", Initializer);
				return Pipeline ? FResult::Success(42) : FResult::Failure(MakeError());
			}, [](const auto&) { ADD_FAILURE() << "Admission deferral is not failure."; }), nullptr);
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Pending);
			EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
			EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::WaitUnavailable);
			EXPECT_EQ(RHI.Creations.load(), 0u);
		}

	}
}
