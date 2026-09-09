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
			auto RHICreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc&) -> FTextureRHIRef override { return {}; }
			auto RHICreateSampler(const FRHISamplerDesc&) -> FSamplerRHIRef override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) -> FShaderRHIRef override { return {}; }
			auto RHICreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc&) -> FBufferRHIRef override { return {}; }
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
				.Message = "injected compile failure",
				.RetryDependencies = RetryDependencies,
			};
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
