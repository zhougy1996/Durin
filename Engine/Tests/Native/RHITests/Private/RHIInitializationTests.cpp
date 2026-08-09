#include <gtest/gtest.h>

#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FInitializationObservation
		{
			uint32 InitCount = 0;
			uint32 ShutdownCount = 0;
			uint32 DestructionCount = 0;
			bool bInitOnRHIThread = false;
			bool bShutdownOnRHIThread = false;
			bool bCapabilitiesClearedAtShutdown = false;
		};

		class FFailingDynamicRHI final : public FDynamicRHI
		{
		public:
			explicit FFailingDynamicRHI(
				FInitializationObservation& InObservation,
				bool bInFailInit = true)
				: Observation(InObservation)
				, bFailInit(bInFailInit)
			{
			}

			~FFailingDynamicRHI() override
			{
				++Observation.DestructionCount;
			}

			auto Init() -> void override
			{
				++Observation.InitCount;
				Observation.bInitOnRHIThread = IsInRHIThread();
				if (bFailInit)
				{
					throw std::runtime_error("intentional backend init failure");
				}
				FRHICapabilities Capabilities;
				Capabilities.SupportedTextureDimensions =
					ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube;
				Capabilities.MaxTextureDimension2D = 4096;
				Capabilities.MaxTextureDimensionCube = 2048;
				Capabilities.MaxTextureArrayLayers = 256;
				Capabilities.ColorSampleCounts = ERHISampleCountFlags::Samples1 | ERHISampleCountFlags::Samples4;
				Capabilities.DepthSampleCounts = ERHISampleCountFlags::Samples1;
				PublishCapabilities(Capabilities);
			}

			auto Shutdown() -> void override
			{
				++Observation.ShutdownCount;
				Observation.bShutdownOnRHIThread = IsInRHIThread();
				ClearCapabilities();
				Observation.bCapabilitiesClearedAtShutdown = RHIGetCapabilities() == nullptr;
			}

			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHICreateViewport(
				void*, uint32, uint32, bool, EPixelFormat,
				EViewportPresentModePolicy) const
				-> TRefCountPtr<FRHIViewport> override { return {}; }
			auto RHIResizeViewport(
				FRHIViewport*, uint32, uint32, bool) -> void override {}
			auto RHICreateGraphicsPipelineState(
				FName, const FGraphicsPipelineStateInitializer&)
				-> TRefCountPtr<FRHIGraphicsPipelineState> override { return {}; }
			auto RHIGetDefaultContext() -> IRHICommandContext* override
			{
				return nullptr;
			}
			auto RHIGetViewportBackBuffer(FRHIViewport*)
				-> TRefCountPtr<FRHITexture> override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&)
				-> TRefCountPtr<FRHIVertexDeclaration> override { return {}; }
			auto RHIIsTextureSupported(
				const FRHITextureCreateDesc&) const -> bool override { return false; }
			auto RHICreateTexture(
				FRHICommandListBase&, const FRHITextureCreateDesc&)
				-> TRefCountPtr<FRHITexture> override { return {}; }
			auto RHICreateSampler(const FRHISamplerDesc&)
				-> TRefCountPtr<FRHISampler> override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&)
				-> TRefCountPtr<FRHIShader> override { return {}; }
			auto RHICreateBuffer(
				FRHICommandListImmediate&, const FRHIBufferCreateDesc&)
				-> TRefCountPtr<FRHIBuffer> override { return {}; }

		private:
			FInitializationObservation& Observation;
			bool bFailInit = true;
		};
	}

	TEST(FRHIInitializationTests,
		ThreadLaunchFailureReleasesUninitializedBackend)
	{
		FInitializationObservation Observation;
		EXPECT_FALSE(RHIInitWithBackendForTests(
			new FFailingDynamicRHI(Observation), true, true));

		EXPECT_EQ(GDynamicRHI, nullptr);
		EXPECT_EQ(Observation.InitCount, 0u);
		EXPECT_EQ(Observation.ShutdownCount, 0u);
		EXPECT_EQ(Observation.DestructionCount, 1u);
		EXPECT_EQ(GetLastRHIInitializationDiagnostic(),
			"Failed to start RHI thread.");
	}

	TEST(FRHIInitializationTests,
		BackendInitFailureRollsBackOnRHIThread)
	{
		FInitializationObservation Observation;
		EXPECT_FALSE(RHIInitWithBackendForTests(
			new FFailingDynamicRHI(Observation), true));

		EXPECT_EQ(GDynamicRHI, nullptr);
		EXPECT_EQ(Observation.InitCount, 1u);
		EXPECT_EQ(Observation.ShutdownCount, 1u);
		EXPECT_EQ(Observation.DestructionCount, 1u);
		EXPECT_TRUE(Observation.bInitOnRHIThread);
		EXPECT_TRUE(Observation.bShutdownOnRHIThread);
		EXPECT_EQ(GetLastRHIInitializationDiagnostic(),
			"intentional backend init failure");
	}

	TEST(FRHIInitializationTests, ExecutionModeDefaultsThreadedAndRetainsInlineOverride)
	{
		EXPECT_EQ(ResolveRHIExecutionMode(nullptr), ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("inline"), ERHIExecutionMode::Inline);
		EXPECT_EQ(ResolveRHIExecutionMode("threaded"),
			ERHIExecutionMode::Threaded);
	}

	TEST(FRHIInitializationTests, CapabilitySnapshotPublishesOnlyAfterSuccessfulInit)
	{
		FInitializationObservation Observation;
		auto* Backend = new FFailingDynamicRHI(Observation, false);
		EXPECT_EQ(Backend->RHIGetCapabilities(), nullptr);
		Backend->Init();
		const FRHICapabilities* Capabilities = Backend->RHIGetCapabilities();
		ASSERT_NE(Capabilities, nullptr);
		EXPECT_EQ(Capabilities->FeatureLevel, ERHIFeatureLevel::ES3_1);
		EXPECT_EQ(Capabilities->SupportedTextureDimensions,
			ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube);
		EXPECT_EQ(Capabilities->MaxTextureDimension2D, 4096u);
		EXPECT_FALSE(Capabilities->bSupportsSynchronization2);

		Backend->Shutdown();
		EXPECT_TRUE(Observation.bCapabilitiesClearedAtShutdown);
		delete Backend;
		EXPECT_EQ(Observation.DestructionCount, 1u);
	}

	TEST(FRHIInitializationTests, InvalidExecutionModeFallsBackThreaded)
	{
		EXPECT_EQ(ResolveRHIExecutionMode(""), ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("THREADed"),
			ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("unexpected"),
			ERHIExecutionMode::Threaded);
	}
} // namespace Durin
