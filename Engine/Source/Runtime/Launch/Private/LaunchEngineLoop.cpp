#include "LaunchEngineLoop.h"

#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"
#include "DogeEdGlobals.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Misc/AppConfigCache.h"
#include "Misc/Paths.h"
#include "Misc/StringConvert.h"

#include "Shader/ShaderPaths.h"
#include "Shader/ShaderCompiler.h"
#include "AssetCore.h"

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

		CoreInternal::LoadApplicationConfig(FPaths::LaunchDir() + "DogeConfig.yaml");

		FNameInit(); // Initialize FName system.
		LoggerInit();
		DOGE_DEBUG("Application name: {}", GAppConfig.GetString("AppName"));
		DOGE_INFO(STR("Launching Doge engine..."));
		DOGE_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DOGE_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		PathUtilities::InitDefaultMountPoints(); // Initialize default mount points to enable path resolving.

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
	}

	struct FPipelineData
	{
		FName PipelineName;
		std::vector<std::vector<uint32>> CompiledCodes;
	};

	struct FTestRenderProxy
	{
		FVertexDeclarationRHIRef VertexDeclaration;
		FBufferRHIRef VertexPositionBuffer;
		FBufferRHIRef VertexColorBuffer;
		FBufferRHIRef IndexBuffer;

		FShaderRHIRef VertexShader;
		FShaderRHIRef PixelShader;

		std::vector<FBufferRHIRef> UniformBuffers;

		std::vector<Asset::FTestAssetData> TestAssetDatas;
	};

	struct FTestUniformBufferObject
	{
		glm::mat4 Model;
		glm::mat4 View;
		glm::mat4 Proj;
	};

	FTestRenderProxy GTestRenderProxy; // Render thread data that will be used to store render resources, and other data that can only be accessed from render thread

	class FTestRenderData
	{
	public:
		auto Prepare() -> void
		{
			std::string ShaderName = "/Engine/Test";
			std::string ShaderSourceFilePath = FShaderPaths::SourcePath(ShaderName);
			std::array<const char8*, 2> EntryPoints = {"vertexMain", "fragmentMain"};
			if (GShaderCompiler->Compile(ShaderSourceFilePath.c_str(), EntryPoints, PipelineData.CompiledCodes))
			{
				for (size_t i = 0; i < EntryPoints.size(); ++i)
				{
					std::string CompiledSpvFilePath = FShaderPaths::BinaryPath(ShaderName, EntryPoints[i], std::hash<std::string_view>{}(ShaderName));
					// FFileHelper::SaveArrayToFile(PipelineData.CompiledCodes[i], CompiledSpvFilePath);
				}
			}

			PipelineData.PipelineName = "TestPipeline";

			auto& App = Mona::FMonaApplication::Get();
			const auto Renderer = dynamic_cast<Mona::FMonaRHIRenderer*>(App.GetRenderer());
			const std::shared_ptr<Mona::MWindow> MainWindow = App.GetActiveTopLevelWindow();
			FViewportRHIRef Viewport = Renderer->GetRHIViewport(*MainWindow);

			// Prepare data before creating render resources, because we need to compile shaders and load assets on the main thread
			std::string TestAssetFilePath = FPaths::Resolve("/Engine/Test/teapot.obj");
			Asset::ImportFromFile(TestAssetFilePath, GTestRenderProxy.TestAssetDatas);

			// Normal to color
			for (auto& AssetData : GTestRenderProxy.TestAssetDatas)
			{
				AssetData.Colors.reserve(AssetData.Normals.size());
				for (const auto& Normal : AssetData.Normals)
				{
					glm::vec3 Color = (Normal + glm::vec3(1.0f)) * 0.5f; // Map normal from [-1, 1] to [0, 1] for visualization
					AssetData.Colors.push_back(Color);
				}
			}

			// Create pipeline
			FPipelineData LocalPipelineData = PipelineData;
			ENQUEUE_RENDER_COMMAND(Pipeline)([Viewport, LocalPipelineData](FRHICommandListImmediate& CommandList) {
				FRHIShaderCreateDesc VertexShaderCreateDesc = FRHIShaderCreateDesc::CreateVertex("TestVertexShader", LocalPipelineData.CompiledCodes[0], {});
				GTestRenderProxy.VertexShader = GDynamicRHI->RHICreateShader(VertexShaderCreateDesc);

				FRHIShaderCreateDesc PixelShaderCreateDesc = FRHIShaderCreateDesc::CreatePixel("TestPixelShader", LocalPipelineData.CompiledCodes[1], {});
				GTestRenderProxy.PixelShader = GDynamicRHI->RHICreateShader(PixelShaderCreateDesc);

				FVertexDeclarationElementList VertexDeclElements;
				VertexDeclElements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, sizeof(glm::vec3));
				VertexDeclElements[1] = FVertexElement(1, 0, EVertexElementType::Float3, 1, sizeof(glm::vec3));
				GTestRenderProxy.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderPassName = "TestRenderPass";
				Initializer.BoundShaders.VertexShader = GTestRenderProxy.VertexShader;
				Initializer.BoundShaders.PixelShader = GTestRenderProxy.PixelShader;
				Initializer.VertexDeclaration = GTestRenderProxy.VertexDeclaration;

				Initializer.PixelFormat = Viewport->GetFormat();
				GDynamicRHI->RHICreateGraphicsPipelineState(LocalPipelineData.PipelineName, Initializer);
				CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			});

			ENQUEUE_RENDER_COMMAND(CreateVertexBuffer)([](FRHICommandListImmediate& CommandList) {
				{
					const auto& TestVertices = GTestRenderProxy.TestAssetDatas[0].Positions;
					uint32 BufferSize = TestVertices.size() * sizeof(glm::vec3);
					FRHIBufferCreateDesc BufferCreateDesc = FRHIBufferCreateDesc::CreateVertex("TestPositionVertexBuffer", BufferSize);
					BufferCreateDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer;
					BufferCreateDesc.InitialData = FResourceArrayUploadInfo{&TestVertices[0], BufferSize};

					GTestRenderProxy.VertexPositionBuffer = RHICreateBuffer(BufferCreateDesc);
				}

				{
					const auto& TestColors = GTestRenderProxy.TestAssetDatas[0].Colors;
					uint32 BufferSize = TestColors.size() * sizeof(glm::vec3);
					FRHIBufferCreateDesc BufferCreateDesc = FRHIBufferCreateDesc::CreateVertex("TestVertexColorBuffer", BufferSize);
					BufferCreateDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer;
					BufferCreateDesc.InitialData = FResourceArrayUploadInfo{&TestColors[0], BufferSize};

					GTestRenderProxy.VertexColorBuffer = RHICreateBuffer(BufferCreateDesc);
				}
			});

			ENQUEUE_RENDER_COMMAND(CreateIndexBuffer)([](FRHICommandListImmediate& CommandList) {
				const auto& TestIndices = GTestRenderProxy.TestAssetDatas[0].Indices;
				uint32 BufferSize = TestIndices.size() * sizeof(uint32);

				FRHIBufferCreateDesc BufferCreateDesc = FRHIBufferCreateDesc::CreateIndex("TestIndexBuffer", BufferSize, sizeof(uint32));
				BufferCreateDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::IndexBuffer;
				BufferCreateDesc.InitialData = FResourceArrayUploadInfo{&TestIndices[0], BufferSize};

				GTestRenderProxy.IndexBuffer = RHICreateBuffer(BufferCreateDesc);
			});

			ENQUEUE_RENDER_COMMAND(CreateUniformBuffers)([](FRHICommandListImmediate& CommandList) {
				FTestUniformBufferObject TestUBO;
				TestUBO.Model = rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
				TestUBO.View = glm::mat4(1.0f);
				TestUBO.Proj = glm::mat4(1.0f);
				check(sizeof(FTestUniformBufferObject) % 16 == 0); // Uniform buffer size must be a multiple of 16 bytes

				FRHIBufferCreateDesc BufferCreateDesc = FRHIBufferCreateDesc::Create("TestUniformBuffer", EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Dynamic);
				BufferCreateDesc.Size = sizeof(FTestUniformBufferObject);
				BufferCreateDesc.InitialData = FResourceArrayUploadInfo{&TestUBO, sizeof(FTestUniformBufferObject)};

				GTestRenderProxy.UniformBuffers.push_back(RHICreateBuffer(BufferCreateDesc));
				GTestRenderProxy.UniformBuffers.push_back(RHICreateBuffer(BufferCreateDesc));
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

				FRHIShaderParameterResource ShaderParameterResource;
				ShaderParameterResource.Type = FRHIShaderParameterResource::EType::UniformBuffer;
				ShaderParameterResource.SetIndex = 0;
				ShaderParameterResource.BindIndex = 0;
				ShaderParameterResource.Resource = GTestRenderProxy.UniformBuffers[GFrameCounterRenderThread % GTestRenderProxy.UniformBuffers.size()];

				static const auto GStartTime = std::chrono::steady_clock::now();
				float Time = std::chrono::duration<float>(std::chrono::steady_clock::now() - GStartTime).count();
				FTestUniformBufferObject TmpUBO;
				TmpUBO.Model = rotate(glm::mat4(1.0f), glm::radians(50.0f * Time), glm::vec3(0.0f, 0.0f, 1.0f));
				TmpUBO.View = glm::mat4(1.0f);
				TmpUBO.Proj = glm::mat4(1.0f);

				CommandList.WriteBuffer(GTestRenderProxy.UniformBuffers[GFrameCounterRenderThread % GTestRenderProxy.UniformBuffers.size()], &TmpUBO, sizeof(FTestUniformBufferObject), 0);

				std::vector<FRHIShaderParameterResource> ShaderParameterResources = {ShaderParameterResource};
				CommandList.SetShaderParameters(GTestRenderProxy.VertexShader, ShaderParameterResources);
				CommandList.BindVertexBuffer(0, GTestRenderProxy.VertexPositionBuffer, 0);
				CommandList.BindVertexBuffer(1, GTestRenderProxy.VertexColorBuffer, 0);
				CommandList.BindIndexBuffer(GTestRenderProxy.IndexBuffer, 0);
				// Draw call
				CommandList.DrawIndexed(GTestRenderProxy.TestAssetDatas[0].Indices.size(), 0, 0);

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
		DOGE_INFO(STR("Doge engine initialized."));
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

		// FPS counter: accumulate frames and log once per second
		{
			static uint64 FPSFrameCount = 0;
			static std::chrono::steady_clock::time_point FPSLastTime = std::chrono::steady_clock::now();

			++FPSFrameCount;
			auto Now = std::chrono::steady_clock::now();
			auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Now - FPSLastTime).count();
			if (Elapsed >= 1000)
			{
				double FPS = static_cast<double>(FPSFrameCount) * 1000.0 / static_cast<double>(Elapsed);
				DOGE_DEBUG("FPS: {:.1f}", FPS);
				FPSFrameCount = 0;
				FPSLastTime = Now;
			}
		}
	}

	auto FEngineLoop::Exit() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseTestProxy)([](FRHICommandListImmediate& RHICmdList) {
			GTestRenderProxy.VertexPositionBuffer = nullptr;
			GTestRenderProxy.VertexColorBuffer = nullptr;
			GTestRenderProxy.IndexBuffer = nullptr;
			GTestRenderProxy.VertexShader = nullptr;
			GTestRenderProxy.PixelShader = nullptr;
			GTestRenderProxy.UniformBuffers.clear();
		});

		ShutdownRenderingThread();
		// TODO: this is just for testing, we should have a more robust shutdown process
		delete GEngine;
		GDynamicRHI->Shutdown();
		Mona::FMonaApplication::Shutdown();

		FModuleManager::Get().UnloadModulesAtShutdown();

		DOGE_INFO(STR("Doge engine exited."));
	}
} // namespace Doge