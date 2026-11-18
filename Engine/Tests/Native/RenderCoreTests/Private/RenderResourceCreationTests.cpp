#include "Shader/ShaderMapPreparation.h"
#include "Threading/TaskRuntime.h"
#include <gtest/gtest.h>

#include "RenderResourceCreation.h"
#include "RenderResourcePreparation.h"
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
		struct FTestPreparation final : FRenderPreparationDependency
		{
			bool bPending = false;
			auto IsPending() const -> bool override { return bPending; }
		};

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
			auto RHICreateGraphicsPipelineState(FName, const FGraphicsPipelineStateInitializer&) noexcept -> FGraphicsPipelineStateRHIRef override
			{
				ADD_FAILURE();
				return {};
			}
			auto RHICreateComputePipelineState(FName, const FComputePipelineStateInitializer&) noexcept -> FComputePipelineStateRHIRef override
			{
				ADD_FAILURE();
				return {};
			}
			auto RHIGetDefaultContext() -> IRHICommandContext* override { return nullptr; }
			auto RHIGetViewportBackBuffer(FRHIViewport*) -> FTextureRHIRef override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&) noexcept -> FVertexDeclarationRHIRef override { return {}; }
			auto RHIIsTextureSupported(const FRHITextureCreateDesc&) const -> bool override { return false; }
			auto RHICreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc&) noexcept -> FTextureRHIRef override
			{
				return {};
			}
			auto RHICreateSampler(const FRHISamplerDesc&) noexcept -> FSamplerRHIRef override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) noexcept -> FShaderRHIRef override { return {}; }
			auto RHICreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc&) noexcept -> FBufferRHIRef override
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
				EXPECT_EQ(Batch.GetPipelineCount(), 0u);
				EXPECT_TRUE(Batch.HasResourcePending());
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

		TEST(FRenderResourceCreationTests, PendingDependencyPollsWithoutRepeatingFactoryAndResumesOnCompletion)
		{
			FSlot Slot;
			auto Dependency = std::make_shared<FTestPreparation>();
			Dependency->bPending = true;
			uint32 Attempts = 0;
			auto Report = [](const auto&) { ADD_FAILURE() << "Pending is not a failure"; };
			auto Factory = [&] {
				++Attempts;
				FRenderPipelinePreparationScope::RetainPreparation(Dependency);
				return Dependency->bPending ? FResult::Pending() : FResult::Success(42);
			};
			for (int Index = 0; Index < 15; ++Index)
			{
				FRenderPipelinePreparationBatch Batch;
				EXPECT_EQ(Slot.Resolve({}, Factory, Report), nullptr);
				EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
				EXPECT_TRUE(Batch.HasResourcePending());
			}
			EXPECT_EQ(Attempts, 1u);
			Dependency->bPending = false;
			const auto* Result = Slot.Resolve({}, Factory, Report);
			ASSERT_NE(Result, nullptr);
			EXPECT_EQ(*Result, 42);
			EXPECT_EQ(Attempts, 2u);
		}

		TEST(FRenderResourceCreationTests, PartialDependencyProgressRetriesAndTerminalFailureIsNotHidden)
		{
			FSlot Slot;
			auto First = std::make_shared<FTestPreparation>();
			auto Second = std::make_shared<FTestPreparation>();
			First->bPending = Second->bPending = true;
			uint32 Attempts = 0, Diagnostics = 0;
			auto Factory = [&] {
				++Attempts;
				FRenderPipelinePreparationScope::RetainPreparation(First);
				FRenderPipelinePreparationScope::RetainPreparation(Second);
				return First->bPending || Second->bPending ? FResult::Pending() : FResult::Failure(MakeError());
			};
			auto Report = [&](const auto&) { ++Diagnostics; };
			EXPECT_EQ(Slot.Resolve({}, Factory, Report), nullptr);
			First->bPending = false;
			EXPECT_EQ(Slot.Resolve({}, Factory, Report), nullptr);
			EXPECT_EQ(Slot.Resolve({}, Factory, Report), nullptr);
			EXPECT_EQ(Attempts, 2u);
			Second->bPending = false;
			EXPECT_EQ(Slot.Resolve({}, Factory, Report), nullptr);
			EXPECT_EQ(Attempts, 3u);
			EXPECT_EQ(Diagnostics, 1u);
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Failed);
		}

		TEST(FRenderResourceCreationTests, PendingDependencyDoesNotSuppressGenerationChangeOrNestedPending)
		{
			FSlot Parent, Child;
			auto Dependency = std::make_shared<FTestPreparation>();
			Dependency->bPending = true;
			uint32 Attempts = 0;
			auto Report = [](const auto&) {};
			FRenderResourceGeneration Generation;
			auto Factory = [&] {
				++Attempts;
				FRenderPipelinePreparationScope::RetainPreparation(Dependency);
				return FResult::Pending();
			};
			EXPECT_EQ(Child.Resolve(Generation, Factory, Report), nullptr);
			{
				FRenderPipelinePreparationBatch Batch;
				EXPECT_EQ(Parent.Resolve(Generation, [&] {
					EXPECT_EQ(Child.Resolve(Generation, Factory, Report), nullptr);
					return FResult::Success(42);
				}, Report), nullptr);
				EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
				EXPECT_EQ(Parent.GetAvailability(), ERenderResourceAvailability::Pending);
				EXPECT_EQ(Attempts, 1u);
			}
			++Generation.Shader;
			EXPECT_EQ(Child.Resolve(Generation, Factory, Report), nullptr);
			EXPECT_EQ(Attempts, 2u);
			Child.Reset();
			EXPECT_EQ(Child.Resolve(Generation, Factory, Report), nullptr);
			EXPECT_EQ(Attempts, 3u);
		}

		TEST(FRenderResourceCreationTests, PreparationLeasesAreReleasedOnPublicationFailureCancellationResetAndGenerationChange)
		{
			FSlot Slot;
			std::weak_ptr<const FTestPreparation> Lifetime;
			auto Report = [](const auto&) {};
			auto Pend = [&] {
				Slot.Reset();
				auto Lease = std::make_shared<FTestPreparation>();
				Lifetime = Lease;
				EXPECT_EQ(Slot.Resolve({}, [&] {
					FRenderPipelinePreparationScope::RetainPreparation(Lease);
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
			Second = First;
			Second.Reason = ERenderResourceCreateErrorReason::PipelinePreparationRejected;
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

		TEST(FRenderResourceCreationTests, PreparationBatchRetainsLargeBatchesAndKeepsNestedStateIndependent)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			ASSERT_TRUE(InitializeTaskScheduler(2));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			RHI.FailCreate = [](uint32) { return true; };
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
			const auto Create = [&](uint32 Index) -> std::shared_ptr<FPipelineState> {
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
				auto Pipeline = PipelineStateCache::PrecacheComputePipelineState(Initializer, "batch failure");
				EXPECT_TRUE(Pipeline);
				return Pipeline ? Pipeline : nullptr;
			};
			FRenderPipelinePreparationBatch Batch;
			std::shared_ptr<FPipelineState> Last;
			for (size_t Index = 0; Index < 4100u; ++Index)
			{
				Last = Create(static_cast<uint32>(Index));
				FRenderPipelinePreparationBatch::Add(Last);
			}
			FRenderPipelinePreparationBatch::Add(Last);
			EXPECT_EQ(Batch.GetPipelineCount(), 4100u);
			FRenderPipelinePreparationBatch::Add(Create(4100));
			EXPECT_EQ(Batch.GetPipelineCount(), 4101u);
			{
				FRenderPipelinePreparationBatch Nested;
				EXPECT_EQ(Nested.GetPipelineCount(), 0u);
			}
			EXPECT_EQ(Batch.GetPipelineCount(), 4101u);
			std::vector<std::shared_ptr<FPipelineState>> Requests;
			Batch.CollectRequests(Requests);
			EXPECT_EQ(Requests.size(), 4101u);
			for (const auto& Request : Requests) EXPECT_EQ(Request->Wait(), ERHIPipelineState::Failed);
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
		TEST(FRenderResourceCreationTests, PreparationRequestObservesOwnerLifetimeAndKeepsTerminalFailure)
		{
			class FPreparationTask final : public FRunnable
			{
			public:
				auto Run() -> uint32 override
				{
					auto Owner = std::make_shared<const uint8>(0);
					auto Request = FRenderResourcePreparation::Create();
					auto Alias = Request;
					auto Peer = FRenderResourcePreparation::Create();
					Request.Complete_RenderThread(ERenderResourcePreparationState::Ready, {}, Owner);
					Peer.Complete_RenderThread(ERenderResourcePreparationState::Ready, {}, Owner);
					EXPECT_FALSE(Request.NeedsPreparation());
					Alias.RequestCancel();
					Request.Complete_RenderThread(ERenderResourcePreparationState::Ready);
					EXPECT_EQ(Request.GetProgress().State, ERenderResourcePreparationState::Canceled);
					EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Ready);
					Owner.reset();
					Peer.Complete_RenderThread(ERenderResourcePreparationState::Ready, {}, std::make_shared<const uint8>(1));
					Peer.RequestCancel();
					EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Stale);
					EXPECT_EQ(Peer.GetProgress().PipelineRequests, 0u);
					auto Failed = FRenderResourcePreparation::Create();
					Failed.Complete_RenderThread(ERenderResourcePreparationState::Failed);
					Failed.RequestCancel();
					Failed.Retire_RenderThread(ERenderResourcePreparationState::Stale);
					EXPECT_EQ(Failed.GetProgress().State, ERenderResourcePreparationState::Failed);
					auto Reloaded = FRenderResourcePreparation::Create();
					Reloaded.Complete_RenderThread(ERenderResourcePreparationState::Ready);
					AdvanceShaderReloadGeneration();
					Reloaded.Complete_RenderThread(ERenderResourcePreparationState::Ready);
					EXPECT_FALSE(Reloaded.NeedsPreparation());
					EXPECT_EQ(Reloaded.GetProgress().State, ERenderResourcePreparationState::Stale);
					return 0;
				}
			} Task;
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Task, "PreparationRequestLifetime", 0,
				EThreadPriority::Normal, EThreadRole::RenderingThread));
			ASSERT_NE(Thread, nullptr);
			Thread->WaitForCompletion();
		}

		TEST(FRenderResourceCreationTests, PreparedPipelineRequestsSurviveBatchDestruction)
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
					std::weak_ptr<FPipelineState> PipelineLifetime;
					auto Preparation = FRenderResourcePreparation::Create();
					FThreadEvent Entered, Release;
					RHI.BeforeCreate = [&](uint32) { Entered.Trigger(); Release.Wait(); };
					{
						FRenderPipelinePreparationBatch Batch;
						auto Request = PipelineStateCache::PrecacheComputePipelineState(Initializer, "retained");
						EXPECT_TRUE(Request);
						if (!Request) { RHI.BeforeCreate = {}; return 1; }
						FRenderPipelinePreparationBatch::Add(Request);
						std::vector<std::shared_ptr<FPipelineState>> Requests;
						Batch.CollectRequests(Requests);
						EXPECT_EQ(Requests.size(), 1u);
						if (Requests.size() != 1u) { Release.Trigger(); (void)Request->Wait(); RHI.BeforeCreate = {}; return 1; }
						PipelineLifetime = Requests.front();
						auto Peer = FRenderResourcePreparation::Create();
						Peer.Complete_RenderThread(ERenderResourcePreparationState::Ready, Requests);
						Preparation.Complete_RenderThread(ERenderResourcePreparationState::Ready, std::move(Requests));
						EXPECT_TRUE(Entered.WaitFor(5.0));
						EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Pending);
						EXPECT_FALSE(Preparation.NeedsPreparation());
						// A duplicate completion cannot discard accepted pending handles.
						Preparation.Complete_RenderThread(ERenderResourcePreparationState::Ready);
						EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Pending);
						Peer.RequestCancel();
						EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Canceled);
						EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Pending);
					}
					Release.Trigger();
					const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
					while (Preparation.GetProgress().State == ERenderResourcePreparationState::Pending && std::chrono::steady_clock::now() < Deadline)
						std::this_thread::yield();
					RHI.BeforeCreate = {};
					EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Ready);
					EXPECT_EQ(RHI.Creations.load(), 1u);
					auto Activated = PipelineStateCache::PrecacheComputePipelineState(Initializer, "retained");
					EXPECT_NE(Activated, nullptr);
					if (!Activated) return 1;
					EXPECT_EQ(RHI.Creations.load(), 1u);
					Preparation.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
					EXPECT_EQ(Activated->GetState(), ERHIPipelineState::Ready);
					EXPECT_FALSE(PipelineLifetime.expired());
					Activated.reset();
					EXPECT_TRUE(PipelineLifetime.expired());
					RHI.FailCreate = [](uint32) { return true; };
					Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 1;
					auto Failed = FRenderResourcePreparation::Create();
					auto Retired = FRenderResourcePreparation::Create();
					std::shared_ptr<FPipelineState> FailedPipeline;
					{
						FRenderPipelinePreparationBatch Batch;
						auto Request = PipelineStateCache::PrecacheComputePipelineState(Initializer, "failure");
						EXPECT_TRUE(Request);
						if (!Request) { RHI.BeforeCreate = {}; return 1; }
						FRenderPipelinePreparationBatch::Add(Request);
						std::vector<std::shared_ptr<FPipelineState>> Requests;
						Batch.CollectRequests(Requests);
						EXPECT_EQ(Requests.size(), 1u);
						if (Requests.size() != 1u) return 1;
						FailedPipeline = Requests.front();
						Retired.Complete_RenderThread(ERenderResourcePreparationState::Ready, Requests);
						Failed.Complete_RenderThread(ERenderResourcePreparationState::Ready, std::move(Requests));
					}
					const auto FailureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
					while (FailedPipeline->GetState() == ERHIPipelineState::Pending && std::chrono::steady_clock::now() < FailureDeadline)
						std::this_thread::yield();
					// No view-request poll precedes cancellation or retirement.
					Failed.RequestCancel();
					Retired.Retire_RenderThread(ERenderResourcePreparationState::Stale);
					EXPECT_EQ(Retired.GetProgress().State, ERenderResourcePreparationState::Failed);
					EXPECT_EQ(Failed.GetProgress().State, ERenderResourcePreparationState::Failed);
					return 0;
				}
			} Task(RHI);
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Task, "PreparationEviction", 0,
				EThreadPriority::Normal, EThreadRole::RenderingThread));
			ASSERT_NE(Thread, nullptr); Thread->WaitForCompletion();
		}


	}
}
