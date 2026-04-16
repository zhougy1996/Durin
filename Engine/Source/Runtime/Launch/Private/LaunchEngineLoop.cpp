#include "LaunchEngineLoop.h"

#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/ConfigCacheJson.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"
#include "DogeEdGlobals.h"
#include "Engine/Engine.h"
#include "Misc/FileHelper.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/StringConvert.h"

#include "Shader/ShaderCompiler.h"

namespace Doge
{
	FEngineLoop GEngineLoop;

	constexpr auto DLLModuleDependencies = std::array{"MainFrame"};

	auto FEngineLoop::PreInit() -> void
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		AddDllDirectory(StringConvert::Utf8ToWide(FPaths::EngineThirdPartyRuntimeBinariesDir()).c_str());

		DOGE_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DOGE_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());

		FNameInit();
		GlobalConfigsInit();

		LoggerInit();
		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
	}
	// Test code

	struct FPipelineData
	{
		FName PipelineName;
		FName VertexShaderPath;
		FName PixelShaderPath;
	};

	struct FTestRenderProxy
	{
		FVertexDeclarationRHIRef VertexDeclaration;
		FBufferRHIRef VertexBuffer;
		FBufferRHIRef IndexBuffer;
	};

	FTestRenderProxy GTestRenderProxy; // Render thread data that will be used to store render resources, and other data that can only be accessed from render thread

	class FTestRenderData
	{
	public:
		auto Prepare() -> void
		{
			// Test compiler
			std::string ShaderFilename = FPaths::EngineDir() + "Shaders/Slang/HelloWorld.slang";
			GShaderCompiler->CompileShader(ShaderFilename.c_str(), "computeMain");

			PipelineData.PipelineName = "TestPipeline";

			std::string ShaderDir = FPaths::EngineDir() + "Shaders/spv/";
			PipelineData.VertexShaderPath = ShaderDir + "test_vert.spv";
			PipelineData.PixelShaderPath = ShaderDir + "test_frag.spv";

			auto& App = Mona::FMonaApplication::Get();
			const auto Renderer = dynamic_cast<Mona::FMonaRHIRenderer*>(App.GetRenderer());
			const std::shared_ptr<Mona::MWindow> MainWindow = App.GetActiveTopLevelWindow();
			const FRHIViewport* Viewport = Renderer->GetRHIViewport(*MainWindow).GetReference();
			EPixelFormat ViewportFormat = Viewport->GetFormat();
			// Create pipeline

			FPipelineData LocalPipelineData = PipelineData;
			ENQUEUE_RENDER_COMMAND(Pipeline)([ViewportFormat, LocalPipelineData](FRHICommandListImmediate& CommandList) {
				std::vector<uint32> VertexShaderCode;
				std::vector<uint32> PixelShaderCode;
				FFileHelper::LoadFileToArray(VertexShaderCode, LocalPipelineData.VertexShaderPath.ToString());
				FFileHelper::LoadFileToArray(PixelShaderCode, LocalPipelineData.PixelShaderPath.ToString());

				FRHIShaderCreateDesc VertexShaderCreateDesc = FRHIShaderCreateDesc::CreateVertex("TestVertexShader", VertexShaderCode, {});
				auto VertexTestShader = GDynamicRHI->RHICreateShader(VertexShaderCreateDesc);

				FRHIShaderCreateDesc PixelShaderCreateDesc = FRHIShaderCreateDesc::CreatePixel("TestPixelShader", PixelShaderCode, {});
				auto PixelTestShader = GDynamicRHI->RHICreateShader(PixelShaderCreateDesc);

				FVertexDeclarationElementList VertexDeclElements;
				VertexDeclElements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, sizeof(glm::vec3));
				GTestRenderProxy.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderPassName = "TestRenderPass";
				Initializer.BoundShaders.VertexShader = VertexTestShader;
				Initializer.BoundShaders.PixelShader = PixelTestShader;
				Initializer.VertexDeclaration = GTestRenderProxy.VertexDeclaration;

				Initializer.PixelFormat = ViewportFormat;
				GDynamicRHI->RHICreateGraphicsPipelineState(LocalPipelineData.PipelineName, Initializer);
				CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			});

			ENQUEUE_RENDER_COMMAND(CreateVertexBuffer)([](FRHICommandListImmediate& CommandList) {
				const std::vector<glm::vec3> TestVertices = {
					{-0.5f, -0.5f, 0.0f},
					{0.5f, -0.5f, 0.0f},
					{0.5f, 0.5f, 0.0f},
					{-0.5f, 0.5f, 0.0f}
				};

				FRHIBufferCreateDesc BufferCreateDesc =
					FRHIBufferCreateDesc::CreateVertex("TestVertexBuffer", TestVertices.size() * sizeof(glm::vec3));
				BufferCreateDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer;
				auto VertexBuffer= RHICreateBuffer(BufferCreateDesc);
				void* Data = CommandList.LockBuffer(VertexBuffer, 0, VertexBuffer->GetSize(), EResourceLockMode::WriteOnly);
				std::memcpy(Data, &TestVertices[0], TestVertices.size() * sizeof(glm::vec3));
				CommandList.UnlockBuffer(VertexBuffer);
				GTestRenderProxy.VertexBuffer = VertexBuffer;
			});

			ENQUEUE_RENDER_COMMAND(CreateIndexBuffer)([](FRHICommandListImmediate& CommandList) {
				const std::vector<uint16> TestIndices = { 0, 1, 2, 2, 3, 0 };
				auto BufferSize = TestIndices.size() * sizeof(uint16);
				FRHIBufferCreateDesc BufferCreateDesc =
					FRHIBufferCreateDesc::CreateIndex("TestIndexBuffer", BufferSize, sizeof(uint16));
				BufferCreateDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::IndexBuffer;
				auto IndexBuffer = RHICreateBuffer(BufferCreateDesc);
				void* Data = CommandList.LockBuffer(IndexBuffer, 0, IndexBuffer->GetSize(), EResourceLockMode::WriteOnly);
				std::memcpy(Data, &TestIndices[0], BufferSize);
				CommandList.UnlockBuffer(IndexBuffer);
				GTestRenderProxy.IndexBuffer = IndexBuffer;
			});

			ENQUEUE_RENDER_COMMAND(CreateTexture)([](FRHICommandListImmediate& CommandList) {
				FRHITextureCreateDesc TextureCreateDesc = FRHITextureCreateDesc::Create2D("TestTex", 256, 256, EPixelFormat::RGBA8_UNORM);
				TRefCountPtr<FRHITexture> Texture = RHICreateTexture(TextureCreateDesc);
			});

			bIsReady = true;
		}

		auto Render() -> void
		{
			auto& App = Mona::FMonaApplication::Get();
			const auto Renderer = dynamic_cast<Mona::FMonaRHIRenderer*>(App.GetRenderer());
			const std::shared_ptr<Mona::MWindow> MainWindow = App.GetActiveTopLevelWindow();
			if (!MainWindow || MainWindow->IsMinimized())
			{
				return;
			}
			TRefCountPtr<FRHIViewport> SharedViewport = Renderer->GetRHIViewport(*MainWindow);

			ENQUEUE_RENDER_COMMAND(TestDrawTriangle)([SharedViewport](FRHICommandListImmediate& CommandList) {
				CommandList.SwitchPipeline(ERHIPipeline::Graphics);

				FRHIViewport* Viewport = SharedViewport.GetReference();
				// Draw viewport
				// Wait submit fence before acquiring image
				CommandList.BeginDrawingViewport(Viewport, nullptr);

				// Acquire image
				TRefCountPtr<FRHITexture> BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(Viewport);

				FRHIRenderPassInfo PassInfo{};
				PassInfo.ColorRenderTargets[0] = BackBuffer.GetReference();

				CommandList.BeginRenderPass(PassInfo, "TestRenderPass");

				CommandList.SetGraphicsPipelineState(*GDynamicRHI->RHIGetGraphicsPipelineState("TestPipeline"));

				auto Width = BackBuffer->GetSizeX();
				auto Height = BackBuffer->GetSizeY();
				CommandList.SetViewport(0, 0, 0, static_cast<float>(Width), static_cast<float>(Height), 1.0f);

				CommandList.BindVertexBuffer(0, GTestRenderProxy.VertexBuffer, 0);
				CommandList.BindIndexBuffer(GTestRenderProxy.IndexBuffer, 0);
				// Draw call
				// CommandList.DrawPrimitive();

				CommandList.EndRenderPass();

				// End drawing viewport and present
				CommandList.EndDrawingViewport(Viewport, true, false);
			});
		}

		auto IsReady() const -> bool { return bIsReady; }

		auto Release()
		{
			bIsReady = false;
		}

	private:
		FPipelineData PipelineData;

		bool bIsReady = false;
	};

	FTestRenderData GTestRenderData; // Main thread data that will be used to prepare and submit render commands

	auto FEngineLoop::Init() -> void
	{
		ApplicationInit();
		RHIInit();
		Mona::MonaInit();
		EditorInit();

		// Create engine instance, this is just for testing, we should have a more robust engine initialization process
		GEngine = new DEngine();
		GEngine->Init();

		InitRenderingThread();
	}

	// Called from render thread
	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 FrameCounter) -> void
	{
		check(IsInRenderingThread());
		GFrameCounterRenderThread = FrameCounter;
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		GDynamicRHI->RHIBeginFrame();
	}

	// Called from render thread
	static auto EndFrameRenderThread(FRHICommandListImmediate& RHICmdList, uint64 FrameCounter) -> void
	{
		check(IsInRenderingThread());
		check(GFrameCounterRenderThread == FrameCounter);
		GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
	}

	auto FEngineLoop::Tick() -> void
	{
		uint64 CurrentFrameCounter = GFrameCounter;

		// Game logic.
		GEngine->Tick(0.0f, false);

		// Process application events, and paint UI.
		Mona::FMonaApplication::Get().Tick();

		if (GIsRequestingExit)
		{
			return;
		}

		if (!GTestRenderData.IsReady())
		{
			GTestRenderData.Prepare();
		}

		ENQUEUE_RENDER_COMMAND(BeginFrame)([CurrentFrameCounter](FRHICommandListImmediate& CommandList) {
			BeginFrameRenderThread(CommandList, CurrentFrameCounter);
		});

		GTestRenderData.Render();

		ENQUEUE_RENDER_COMMAND(EndFrame)([CurrentFrameCounter](FRHICommandListImmediate& RHICmdList) {
			EndFrameRenderThread(RHICmdList, CurrentFrameCounter);
		});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		GFrameCounter++;
	}

	auto FEngineLoop::Exit() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseTestProxy)([](FRHICommandListImmediate& RHICmdList) {
			GTestRenderProxy.VertexBuffer = nullptr;
			GTestRenderProxy.IndexBuffer = nullptr;
		});

		ShutdownRenderingThread();
		// TODO: this is just for testing, we should have a more robust shutdown process
		delete GEngine;
		GDynamicRHI->Shutdown();
		Mona::FMonaApplication::Shutdown();

		FModuleManager::Get().UnloadModulesAtShutdown();

		GlobalConfigsDeinit();
	}
} // namespace Doge