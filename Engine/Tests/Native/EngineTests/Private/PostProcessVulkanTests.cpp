#include <gtest/gtest.h>

#include "VulkanEngineTestSupport.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "Shader/GlobalShader.h"
#include "Misc/Paths.h"
#include "Misc/RuntimePaths.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RDG/RDGDiagnostics.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Rendering/SceneInterface.h"
#include "Threading/TaskRuntime.h"

namespace Durin
{

	TEST(FPostProcessVulkanTests, CopyAndFXAAReusePipelinesAndFollowSuccessfulShaderPublication)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		struct FTaskScope { ~FTaskScope() { ShutdownTaskScheduler(); } } TaskScope;
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		// ShaderBuild freezes its mounts on startup. Give this test process its own engine shader tree.
		const auto Engine = Testing::GetTestWorkDirectory() / "Engine";
		const auto Source = Engine / "Shaders" / "Slang";
		std::filesystem::create_directories(Source);
		std::filesystem::copy(std::filesystem::path(FPaths::EngineDir()) / "Shaders" / "Slang", Source,
			std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
		FRuntimePathContext Paths;
		Paths.Bindings = {{"Engine", Engine.generic_string() + "/"},
			{"EngineContent", FPaths::EngineContentDir()}, {"ThirdParty", FPaths::EngineThirdPartyRuntimeBinariesDir()}};
		ASSERT_TRUE(PublishRuntimePathContext(std::move(Paths)));
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
			}
		} Cleanup{Lifecycle, Scene};
		Scene = Renderer.CreateScene();
		auto Prepare = [&](bool FXAA, EPixelFormat Format, FByteBuffer* Pixels = nullptr,
			bool Bloom = false, float Brightness = 0.0f, FRDGCapture* Capture = nullptr) {
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
							.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
					ASSERT_TRUE(Output);
					FSceneView View;
					View.ViewportWidth = 32; View.ViewportHeight = 32;
					View.Settings.Mode.RenderMode = ERenderMode::Unlit;
					View.Settings.PostProcess.bEnableFXAA = FXAA;
					View.Settings.PostProcess.bEnableBloom = Bloom;
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
			if (Pixels)
			{
				EnqueueRenderCommand("DrawPostProcess", [&](FRHICommandListImmediate& Commands) {
					++GRenderFrameCounterRenderThread;
					Commands.SwitchPipeline(ERHIPipeline::Graphics);
					GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
					Renderer.BeginFrame_RenderThread(Commands);
					FSceneView View;
					View.ViewportWidth = 32;
					View.ViewportHeight = 32;
					View.ClearColor = {Brightness, Brightness, Brightness, 1.0f};
					View.Settings.Mode.RenderMode = ERenderMode::Unlit;
					View.Settings.PostProcess.bEnableFXAA = FXAA;
					View.Settings.PostProcess.bEnableBloom = Bloom;
					EXPECT_EQ(Renderer.RenderView(Commands, {Scene.get(), View, Output, false, {}, nullptr, Capture}), ERenderViewResult::Success);
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Output, 0, 0, *Pixels));
					GDynamicRHI->RHIEndFrame_RenderThread(Commands);
				});
				FlushRenderingCommands();
			}
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
		Prepare(false, EPixelFormat::SRGBA8_UNORM);
		Prepare(true, EPixelFormat::SBGRA8_UNORM);
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, Expected);

		auto CheckDraws = [&](uint8 CopyAlpha, uint8 FXAAAlpha) {
			for (bool FXAA : {false, true})
			{
				FByteBuffer Pixels;
				Prepare(FXAA, EPixelFormat::SRGBA8_UNORM, &Pixels);
				ASSERT_EQ(Pixels.size(), 32u * 32u * 4u);
				EXPECT_NEAR(std::to_integer<int>(Pixels[3]), FXAA ? FXAAAlpha : CopyAlpha, 1);
			}
		};
		CheckDraws(255, 255);
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, Expected);
		for (const bool FXAA : {false, true})
		{
			SCOPED_TRACE(FXAA);
			FByteBuffer Disabled, Enabled, DimDisabled, DimEnabled;
			FRDGCapture DisabledGraph, EnabledGraph;
			Prepare(FXAA, EPixelFormat::SRGBA8_UNORM, &Disabled, false, 1.75f, &DisabledGraph);
			Prepare(FXAA, EPixelFormat::SRGBA8_UNORM, &Enabled, true, 1.75f, &EnabledGraph);
			Prepare(FXAA, EPixelFormat::SRGBA8_UNORM, &DimDisabled, false, 0.25f);
			Prepare(FXAA, EPixelFormat::SRGBA8_UNORM, &DimEnabled, true, 0.25f);
			ASSERT_EQ(Enabled.size(), 32u * 32u * 4u);
			ASSERT_EQ(Disabled.size(), Enabled.size());
			EXPECT_GT(std::to_integer<int>(Enabled[0]), std::to_integer<int>(Disabled[0]));
			EXPECT_EQ(Enabled[3], Disabled[3]);
			EXPECT_EQ(DimEnabled, DimDisabled);
			const auto IsBloom = [](const FRDGPassCapture& Pass) { return Pass.Name.starts_with("Scene.Bloom."); };
			EXPECT_EQ(std::ranges::count_if(DisabledGraph.Passes, IsBloom), 0);
			EXPECT_EQ(std::ranges::count_if(EnabledGraph.Passes, IsBloom), 9);
			const auto LastUpsample = std::ranges::find(EnabledGraph.Passes, "Scene.Bloom.Upsample.0", &FRDGPassCapture::Name);
			const auto Display = std::ranges::find(EnabledGraph.Passes, "Scene.PostProcess", &FRDGPassCapture::Name);
			ASSERT_NE(Display, EnabledGraph.Passes.end());
			EXPECT_LT(LastUpsample, Display);
		}
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, ++Expected);

		const auto Path = Source / "PostProcess.slang";
		std::ifstream Input(Path, std::ios::binary);
		ASSERT_TRUE(Input);
		std::string Code{std::istreambuf_iterator<char>(Input), {}};
		Input.close();
		const std::string Alpha = "DisplayAlpha(center.a)";
		const auto Copy = Code.find(Alpha);
		const auto FXAA = Code.rfind(Alpha);
		ASSERT_NE(Copy, std::string::npos);
		ASSERT_LT(Copy, FXAA);
		Code.replace(FXAA, Alpha.size(), "0.75 * " + Alpha);
		Code.replace(Copy, Alpha.size(), "0.25 * " + Alpha);
		auto Reload = [&](const std::string& Text) {
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			Output << Text;
			Output.close();
			ASSERT_TRUE(Output);
			CompileGlobalShaderMap(true);
			FlushRenderingCommands();
		};
		// Publish without advancing the renderer generation. Only the shaders changed.
		Reload(Code);
		CheckDraws(64, 191);
		Expected += 2;
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, Expected);
		Reload(Code + "\n#error intentional PostProcess reload failure\n");
		CheckDraws(64, 191);
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().GraphicsPipelines.NativeCreations, Expected);

	}

}
