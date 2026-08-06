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
		};

		class FFailingDynamicRHI final : public FDynamicRHI
		{
		public:
			explicit FFailingDynamicRHI(FInitializationObservation& InObservation)
				: Observation(InObservation)
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
				throw std::runtime_error("intentional backend init failure");
			}

			auto Shutdown() -> void override
			{
				++Observation.ShutdownCount;
				Observation.bShutdownOnRHIThread = IsInRHIThread();
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
			auto RHIIsTextureFormatSupported(
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

	TEST(FRHIInitializationTests, InvalidExecutionModeFallsBackThreaded)
	{
		EXPECT_EQ(ResolveRHIExecutionMode(""), ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("THREADed"),
			ERHIExecutionMode::Threaded);
		EXPECT_EQ(ResolveRHIExecutionMode("unexpected"),
			ERHIExecutionMode::Threaded);
	}
} // namespace Durin
