#include <gtest/gtest.h>

#include "Client/SceneViewport.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHI.h"
#include "RHIContext.h"
#include "RenderingThread.h"
#include "Runtime/MonaImGui/Private/Backend/ImGuiRHIViewportData.h"

namespace Durin
{
	namespace
	{
		class FViewportTestTexture final : public FRHITexture
		{
		public:
			explicit FViewportTestTexture(const FRHITextureDesc& Desc) : FRHITexture(Desc) {}
		};

		class FViewportTestBuffer final : public FRHIBuffer
		{
		public:
			FViewportTestBuffer() : FRHIBuffer(FRHIBufferCreateDesc::CreateVertex("ViewportTestUpload", 4)) {}
		};

		class FViewportTestRHI final : public FDynamicRHI, public IRHICommandContext
		{
		public:
			auto Init(const FRHIInitializationContext&) -> void override {}
			auto Shutdown() -> void override {}
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHISubmitCommands() -> void override {}
			auto RHIGetDefaultContext() -> IRHICommandContext* override { return this; }
			auto RHICreateViewport(const FRHIViewportCreateInfo&) -> FViewportRHIRef override { return {}; }
			auto RHIResizeViewport(FRHIViewport*, uint32, uint32, bool) -> void override {}
			auto RHIGetViewportBackBuffer(FRHIViewport*) -> FTextureRHIRef override { return {}; }
			auto RHICreateGraphicsPipelineState(FName, const FGraphicsPipelineStateInitializer&)
				-> FGraphicsPipelineStateRHIRef override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&)
				-> FVertexDeclarationRHIRef override { return {}; }
			auto RHIIsTextureSupported(const FRHITextureCreateDesc&) const -> bool override { return true; }
			auto RHICreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc& Desc) -> FTextureRHIRef override
			{
				++CreationCount;
				bCreatedOnRenderThread &= IsInRenderingThread();
				// Exercise the same immediate-list lock guard as Vulkan creation.
				GCommandListExecutor.ExecuteSynchronousOperation(false, [] {});
				if (bFailCreation) return {};
				return FTextureRHIRef(new FViewportTestTexture(Desc));
			}
			auto RHICreateSampler(const FRHISamplerDesc&) -> FSamplerRHIRef override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) -> FShaderRHIRef override { return {}; }
			auto RHICreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc&)
				-> FBufferRHIRef override
			{
				return {};
			}
			auto RHIBeginDiagnosticRegion(std::string_view) -> void override {}
			auto RHIEndDiagnosticRegion() -> void override {}
			auto RHIBeginRenderPass(const FRHIRenderPassInfo&, FName) -> void override {}
			auto RHIEndRenderPass() -> void override {}
			auto RHIBeginDrawingViewport(FRHIViewport*, FRHITexture*) -> void override {}
			auto RHIEndDrawingViewport(FRHIViewport*, bool, bool) -> void override {}
			auto RHISetViewport(float, float, float, float, float, float) -> void override {}
			auto RHISetScissor(float, float, float, float) -> void override {}
			auto RHISetDepthBias(float, float, float) -> void override {}
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState&) -> void override {}
			auto RHIBindVertexBuffer(uint32, FRHIBuffer*, uint32) -> void override {}
			auto RHIBindIndexBuffer(FRHIBuffer*, uint32) -> void override {}
			auto RHITransitionBuffers(std::span<const FRHIBufferTransition>) -> void override {}
			auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override
			{
				TransitionCount += Transitions.size();
			}
			auto RHICopyBuffer(FRHIBuffer*, FRHIBuffer*, std::span<const FRHIBufferCopyRegion>) -> void override {}
			auto RHICopyBufferToTexture(FRHIBuffer*, FRHITexture*, std::span<const FRHIBufferTextureCopyRegion>) -> void override {}
			auto RHICopyTextureToBuffer(FRHITexture*, FRHIBuffer*, std::span<const FRHIBufferTextureCopyRegion>) -> void override {}
			auto RHICopyTexture(FRHITexture*, FRHITexture*, std::span<const FRHITextureCopyRegion>) -> void override {}
			auto RHIWriteBuffer(FRHIBuffer*, uint32, Durin::FByteView) -> void override { ++UploadCount; }
			auto RHIInitializeTexture(FRHITexture*) -> void override {}
			auto RHIUpdateTexture2D(FRHITexture*, uint32, uint32, const FUpdateTextureRegion2D&,
				uint32, Durin::FByteView) -> void override {}
			auto RHIUpdateTexture3D(FRHITexture*, uint32, const FUpdateTextureRegion3D&,
				uint32, uint32, Durin::FByteView) -> void override {}
			auto RHIReadTexture2D(FRHITexture*, uint32, uint32, FByteBuffer&) -> bool override { return false; }
			auto RHIAcquireBackBuffer(FRHITexture*) -> void override {}
			auto RHIBlockUntilGPUIdle() -> void override { ++GPUIdleCount; }
			auto RHIPushConstants(EShaderStageFlags, uint32, uint32, const void*) -> void override {}
			auto RHISetShaderParameters(FRHIShader*, const std::span<const FRHIShaderParameterResource>&) -> void override {}
			auto RHIDraw(const FRHIDrawArguments&) -> void override {}
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments&) -> void override {}

			uint32 CreationCount = 0;
			uint32 TransitionCount = 0;
			uint32 UploadCount = 0;
			uint32 GPUIdleCount = 0;
			bool bCreatedOnRenderThread = true;
			bool bFailCreation = false;
		};

		class FSceneViewportResourceTests : public testing::Test
		{
		protected:
			auto SetUp() -> void override
			{
				ASSERT_EQ(GDynamicRHI, nullptr);
				if (!IsFNameInitialized()) FNameInit();
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				GDynamicRHI = &RHI;
				InitRenderingThread();
				bRenderingStarted = true;
			}
			auto TearDown() -> void override
			{
				if (!bRenderingStarted) return;
				FlushRenderingCommands();
				ShutdownRenderingThread();
				GDynamicRHI = nullptr;
			}
			FViewportTestRHI RHI;
			bool bRenderingStarted = false;
		};
	}

	TEST_F(FSceneViewportResourceTests, CreationAndResizeRunAfterEarlierRenderUploads)
	{
		const FBufferRHIRef Buffer = new FViewportTestBuffer();
		ENQUEUE_RENDER_COMMAND(EarlierViewportUpload)([Buffer](FRHICommandListImmediate& CommandList) {
			auto* Bytes = static_cast<uint32*>(CommandList.LockBuffer(Buffer, 0, 4, EResourceLockMode::WriteOnly));
			*Bytes = 123;
			CommandList.UnlockBuffer(Buffer);
		});
		auto Viewport = FSceneViewport::CreateOffscreen(nullptr);
		Viewport->PrepareDisplay({63.5f, 0.0f});
		ASSERT_NE(Viewport->GetDisplayTexture(), nullptr);
		EXPECT_TRUE(RHI.bCreatedOnRenderThread);
		EXPECT_EQ(Viewport->GetDisplayTexture()->GetSizeX(), 64u);
		EXPECT_EQ(Viewport->GetDisplayTexture()->GetSizeY(), 8u);
		const auto First = Viewport->GetDisplayTexture();
		Viewport->PrepareDisplay({64.0f, 8.0f});
		EXPECT_EQ(Viewport->GetDisplayTexture(), First);
		EXPECT_EQ(RHI.CreationCount, 1u);
		Viewport->PrepareDisplay({128.0f, 96.0f});
		ASSERT_NE(Viewport->GetDisplayTexture(), nullptr);
		EXPECT_NE(Viewport->GetDisplayTexture(), First);
		EXPECT_EQ(Viewport->GetDisplayTexture()->GetSizeX(), 128u);
		EXPECT_EQ(RHI.CreationCount, 2u);
		FlushRenderingCommands();
		EXPECT_EQ(RHI.UploadCount, 1u);
		EXPECT_EQ(RHI.TransitionCount, 2u);
		EXPECT_EQ(RHI.GPUIdleCount, 0u);
	}

	TEST_F(FSceneViewportResourceTests, FailedCreationCanRetryAndRenderThreadCallerDoesNotWaitOnItself)
	{
		auto Viewport = FSceneViewport::CreateOffscreen(nullptr);
		RHI.bFailCreation = true;
		Viewport->PrepareDisplay({32.0f, 32.0f});
		EXPECT_EQ(Viewport->GetDisplayTexture(), nullptr);
		RHI.bFailCreation = false;
		ENQUEUE_RENDER_COMMAND(RetryViewportOnRenderThread)([Viewport](FRHICommandListImmediate&) {
			Viewport->PrepareDisplay({32.0f, 32.0f});
		});
		FlushRenderingCommands();
		EXPECT_NE(Viewport->GetDisplayTexture(), nullptr);
		EXPECT_TRUE(RHI.bCreatedOnRenderThread);
		EXPECT_EQ(RHI.CreationCount, 2u);
		EXPECT_EQ(RHI.TransitionCount, 1u);
		EXPECT_EQ(RHI.GPUIdleCount, 0u);
	}

	TEST_F(FSceneViewportResourceTests, ImGuiWindowDestructionDrainsBorrowedDrawSnapshots)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		for (int Cycle = 0; Cycle < 8; ++Cycle)
		{
			ImGuiViewport Viewport;
			auto* Data = MonaImGui::CreateRendererViewportData(&Viewport);
			ImDrawData* Snapshot = &Data->DrawDataSnapshots[0].DrawData;
			Snapshot->DisplaySize = ImVec2(90.0f, 29.0f);
			std::promise<void> Release;
			auto Ready = Release.get_future().share();
			std::atomic<bool> OwnerAlive{true};
			std::atomic<bool> Consumed{false};
			ENQUEUE_RENDER_COMMAND(ConsumeClosingImGuiWindow)(
				[&OwnerAlive, &Consumed, Ready, Snapshot](FRHICommandListImmediate&) {
					Ready.wait();
					// Avoid dereferencing freed memory when exercising the regression.
					if (OwnerAlive.load())
						Consumed = Snapshot->DisplaySize.x == 90.0f
							&& Snapshot->DisplaySize.y == 29.0f;
				});
			std::thread Unblock([&Release] {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				Release.set_value();
			});
			MonaImGui::DestroyRendererViewportData(&Viewport);
			OwnerAlive = false;
			EXPECT_TRUE(Consumed.load());
			EXPECT_EQ(Viewport.RendererUserData, nullptr);
			Unblock.join();
			FlushRenderingCommands();
		}
		ImGui::DestroyContext(Context);
	}
}
