#include <gtest/gtest.h>

#include "RenderResourceCreation.h"
#include "DynamicRHI.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/ThreadEvent.h"
#include "RHICommandList.h"

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
			auto RHITryCreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc&) -> std::expected<FTextureRHIRef, FRHICreationError> override
			{
				return std::unexpected(FRHICreationError{ERHIResourceCreationFailure::Unknown, ERHICreationFailureSource::BackendReturnedNull});
			}
			auto RHICreateSampler(const FRHISamplerDesc&) -> FSamplerRHIRef override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) -> FShaderRHIRef override { return {}; }
			auto RHITryCreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc&) -> std::expected<FBufferRHIRef, FRHICreationError> override
			{
				return std::unexpected(FRHICreationError{ERHIResourceCreationFailure::Unknown, ERHICreationFailureSource::BackendReturnedNull});
			}
		protected:
			auto CreatePipelineCreationBackend() -> FRHIPipelineCreationService::FBackend override
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
						BeforeCreate(Key.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot);
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
				.Cause = FShaderError{.Code = EShaderError::InvalidCompileRequest},
				.RetryDependencies = RetryDependencies,
			};
		}

		TEST(FRenderResourceCreationTests, FingerprintsIgnoreExternalWordingAndRetainSemanticCauses)
		{
			auto First = MakeError();
			First.Cause = FShaderError::FromSlang(ESlangShaderError::Module, "first wording", -7);
			auto Second = First;
			std::get<FShaderError>(Second.Cause).ExternalDiagnostic = "different wording";
			Second.AttemptedGeneration.Shader = 99;
			EXPECT_EQ(First.GetFingerprint(), Second.GetFingerprint());
			std::get<FShaderError>(Second.Cause).NativeStatus = -8;
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
			Second = First;
			std::get<FShaderError>(Second.Cause).ActualIdentity = "/Other/Source";
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
			Second = First;
			std::get<FShaderError>(Second.Cause).CaptureLimit =
				FShaderCaptureLimitContext{EShaderCaptureLimit::Files, 10, 11};
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
			First = Second;
			std::get<FShaderError>(Second.Cause).CaptureLimit->Actual = 12;
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
			Second.Cause = FRHICreationError{.Failure = ERHIResourceCreationFailure::OutOfMemory,
				.Source = ERHICreationFailureSource::NativeBackend, .NativeCode = -2};
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
			First = Second;
			std::get<FRHICreationError>(Second.Cause).NativeCode = -3;
			EXPECT_NE(First.GetFingerprint(), Second.GetFingerprint());
		}

		TEST(FRenderResourceCreationTests, StoredFailureOwnsCauseAfterProducerDestruction)
		{
			FSlot Slot;
			std::optional<FRenderResourceCreateDiagnostic> Report;
			Slot.Resolve({}, [] {
				auto Error = MakeError();
				Error.Cause = FShaderError::FromFileSystem(
					std::filesystem::path("/Temporary/Source"), std::make_error_code(std::errc::permission_denied));
				return FResult::Failure(std::move(Error));
			}, [&](auto Diagnostic) { Report = std::move(Diagnostic); });
			Slot.Reset();
			ASSERT_TRUE(Report && Report->Error);
			const auto& Cause = std::get<FShaderError>(Report->Error->Cause);
			EXPECT_EQ(Cause.Code, EShaderError::FileSystemFailure);
			ASSERT_TRUE(Cause.FileError);
			EXPECT_EQ(Cause.FileError->Path.generic_string(), "/Temporary/Source");
			EXPECT_EQ(Cause.FileError->NativeError, std::make_error_code(std::errc::permission_denied));
			EXPECT_EQ(FormatRenderResourceCreateError(*Report->Error), FormatShaderError(Cause));
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

		TEST(FRenderResourceCreationTests, PreparationBatchReportsTerminalResultsAndJoinsAllRequests)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			RHI.BeforeCreate = [](uint32 Slot) {
				if (Slot == 0) throw FRHIRecoverableCreationError({
					.Failure = ERHIResourceCreationFailure::OutOfMemory,
					.Source = ERHICreationFailureSource::NativeBackend, .NativeCode = -7});
			};
			auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
			FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader;
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
			auto Failed = RHI.RHIRequestComputePipelineState(Initializer, "failed");
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 1;
			auto Ready = RHI.RHIRequestComputePipelineState(Initializer, "ready");
			ASSERT_TRUE(Failed.IsAccepted());
			ASSERT_TRUE(Ready.IsAccepted());
			{
				FRenderPipelinePreparationBatch Batch;
				EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::Empty);
				FRenderPipelinePreparationBatch::Add(Failed);
				FRenderPipelinePreparationBatch::Add(Ready);
				FRenderPipelinePreparationBatch::Add(Ready);
				EXPECT_EQ(Batch.GetRequestCount(), 2u);
				EXPECT_EQ(Batch.Wait(), ERenderPipelinePreparationWait::Failed);
				EXPECT_EQ(Failed.GetResult().State, ERHIPipelineRequestState::Failed);
				EXPECT_EQ(Ready.GetResult().State, ERHIPipelineRequestState::Ready);
				{
					FRenderPipelinePreparationBatch Nested;
					EXPECT_FALSE(FRenderPipelinePreparationBatch::HasPending());
					FRenderPipelinePreparationBatch::Add(Ready);
					EXPECT_EQ(Nested.Wait(), ERenderPipelinePreparationWait::Ready);
				}
				EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
			}
			EXPECT_FALSE(FRenderPipelinePreparationBatch::HasPending());
			EXPECT_EQ(RHI.Creations, 2u);
		}

		TEST(FRenderResourceCreationTests, AsyncPipelineFailurePreservesNativeCause)
		{
			ASSERT_EQ(GDynamicRHI, nullptr);
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			struct FCoreGuard { ~FCoreGuard() { GDynamicRHI = nullptr; ShutdownTaskScheduler(); RHIFlushDeferredResources(); } } CoreGuard;
			FPipelineSlotTestRHI RHI;
			GDynamicRHI = &RHI;
			RHI.BeforeCreate = [](uint32) {
				throw FRHIRecoverableCreationError({.Failure = ERHIResourceCreationFailure::OutOfMemory,
					.Source = ERHICreationFailureSource::NativeBackend, .NativeCode = -7});
			};
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
			const auto* Cause = std::get_if<FRHICreationError>(&Slot.GetFailure()->Cause);
			ASSERT_NE(Cause, nullptr);
			EXPECT_EQ(Cause->Failure, ERHIResourceCreationFailure::OutOfMemory);
			EXPECT_EQ(Cause->Source, ERHICreationFailureSource::NativeBackend);
			EXPECT_EQ(Cause->NativeCode, -7);
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
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Creating);
			EXPECT_EQ(RHI.RHIGetPipelineCreationStatistics().ActiveObservers, 1u);
			++Generation.Shader;
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 1;
			EXPECT_EQ(Resolve(), nullptr);
			Release.Trigger();
			auto Current = RHI.RHIRequestComputePipelineState(Initializer, "wait current");
			EXPECT_TRUE(Current.Wait());
			ASSERT_NE(Resolve(), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), Current.GetResult().Compute);
			EXPECT_EQ(RHI.Creations.load(), 2u);
			const auto Old = *Slot.GetPayload();
			++Generation.Manual;
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 2;
			EXPECT_EQ(*Resolve(), Old);
			EXPECT_TRUE(RefreshEntered.WaitFor(5.0));
			EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Refreshing);
			++Generation.Device;
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 3;
			EXPECT_EQ(Resolve(), nullptr);
			RefreshRelease.Trigger();
			auto Latest = RHI.RHIRequestComputePipelineState(Initializer, "wait latest");
			EXPECT_TRUE(Latest.Wait());
			ASSERT_NE(Resolve(), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), Latest.GetResult().Compute);
			EXPECT_NE(*Slot.GetPayload(), Old);
			EXPECT_EQ(Diagnostics, 0u);
			Slot.Reset();
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
			FailedShaderRefreshRetainsLastKnownGoodPayload)
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
						ERenderResourceAvailability::Refreshing);
					EXPECT_EQ(*Slot.GetPayload(), 11);
				}
				return Attempts == 1
					? FResult::Success(11)
					: FResult::Failure(MakeError());
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			++Generation.Shader;
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 11);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::StaleReady);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			EXPECT_TRUE(Slot.GetFailure()->bRetainedFallback);
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
						ERenderResourceAvailability::Refreshing);
					EXPECT_EQ(*Slot.GetPayload(), 4);
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
			DeviceGenerationClearsFallbackBeforeRetry)
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
			EXPECT_FALSE(Slot.GetFailure()->bRetainedFallback);
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
					ERenderResourceAvailability::Creating);
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
			ReentrantRefreshReturnsOnlyLastKnownGoodPayload)
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
							ERenderResourceAvailability::Refreshing);
						int* Fallback = Slot.Resolve(
							Generation,
							[&]() {
								++NestedFactoryCalls;
								return FResult::Success(99);
							},
							Reporter);
						EXPECT_NE(Fallback, nullptr);
						if (Fallback != nullptr)
						{
							EXPECT_EQ(*Fallback, 6);
						}
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
	}
}
