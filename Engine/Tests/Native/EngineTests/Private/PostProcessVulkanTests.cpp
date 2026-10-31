#include <gtest/gtest.h>

#include "VulkanEngineTestSupport.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Rendering/SceneInterface.h"
#include "ShaderPipelineCache.h"
#include "Threading/TaskRuntime.h"

namespace Durin
{

	TEST(FPostProcessVulkanTests, PostProcessPreparesOnlyRequestedVariantsAndReusesCompletedPeers)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		struct FTaskScope { ~FTaskScope() { ShutdownTaskScheduler(); } } TaskScope;
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		FModuleManager::Get().LoadModuleChecked("ShaderBuild");
		RHIInit(Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		FModuleTestHarness Lifecycle("PostProcessDemand");
		Lifecycle.Start(Renderer);
		FScenePtr Scene;
		struct FCleanup
		{
			FModuleTestHarness& Lifecycle;
			FScenePtr& Scene;
			FShaderPipelineCache::BatchMode PreviousMode;
			~FCleanup()
			{
				if (Scene)
				{
					Scene->Release();
					Scene.reset();
					FlushRenderingCommands();
				}
				Lifecycle.Shutdown();
				ShutdownRenderingThread();
				FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
				RHIExit();
				FModuleManager::Get().ShutdownModule("ShaderBuild");
				FShaderPipelineCache::SetBatchMode(PreviousMode);
			}
		} Cleanup{Lifecycle, Scene, FShaderPipelineCache::GetBatchMode()};
		Scene = Renderer.CreateScene();
		FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Fast);
		auto Prepare = [&](bool FXAA, EPixelFormat Format) {
			FTextureRHIRef Output;
			const auto Preparation = FRenderResourcePreparation::Create();
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			do
			{
				EnqueueRenderCommand("PreparePostProcessDemand", [&](FRHICommandListImmediate& Commands) {
					++GRenderFrameCounterRenderThread;
					Commands.SwitchPipeline(ERHIPipeline::Graphics);
					GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
					Renderer.BeginFrame_RenderThread(Commands);
					if (!Output) Output = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("PostProcessDemand", 32, 32, Format)
							.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
					ASSERT_TRUE(Output);
					FSceneView View;
					View.ViewportWidth = 32; View.ViewportHeight = 32;
					View.Settings.Mode.RenderMode = ERenderMode::Unlit;
					View.Settings.PostProcess.bEnableFXAA = FXAA;
					const auto DrawsBefore = Commands.GetNumRecordedDrawCommands();
					Renderer.PrepareViewResources(Commands, {Scene.get(), View, Output, false, {}}, Preparation);
					EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawsBefore);
					GDynamicRHI->RHIEndFrame_RenderThread(Commands);
				});
				FlushRenderingCommands();
				if (Preparation.GetProgress().State != ERenderResourcePreparationState::Pending) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < Deadline);
			EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Ready);
		};
		const auto Initial = GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations;
		uint64 Expected = Initial;
		for (const auto Format : {EPixelFormat::SRGBA8_UNORM, EPixelFormat::SBGRA8_UNORM})
		for (bool FXAA : {false, true})
		{
			SCOPED_TRACE(static_cast<int>(Format));
			SCOPED_TRACE(FXAA);
			Prepare(FXAA, Format);
			EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, ++Expected);
		}
		FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Background);
		Prepare(false, EPixelFormat::SRGBA8_UNORM);
		Prepare(true, EPixelFormat::SBGRA8_UNORM);
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, Expected);
	}

}
