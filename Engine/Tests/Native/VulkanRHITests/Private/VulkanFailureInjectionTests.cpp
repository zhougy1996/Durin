#include <gtest/gtest.h>

#include "Modules/ModuleManager.h"
#include "PCH.VulkanRHI.h"
#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/SlangShaderCompiler.h"
#include "VulkanRHIPrivate.h"
#include "VulkanSwapchain.h"
#include "VulkanViewport.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		class FVulkanCreateFailureInjectionTests : public testing::Test
		{
		protected:
			auto SetUp() -> void override
			{
				if (const char* ExistingMode = std::getenv("DURIN_RHI_EXECUTION"))
				{
					PreviousExecutionMode = ExistingMode;
				}
				_putenv_s("DURIN_RHI_EXECUTION", "threaded");
				ResetVulkanCreateFailures();
			}

			auto TearDown() -> void override
			{
				if (GDynamicRHI)
				{
					RHIExit();
				}
				FModuleManager::Get().UnloadModule("VulkanRHI");
				ResetVulkanCreateFailures();
				_putenv_s("DURIN_RHI_EXECUTION",
					PreviousExecutionMode ? PreviousExecutionMode->c_str() : "");
			}

			std::optional<std::string> PreviousExecutionMode;
		};
	}

	TEST_F(FVulkanCreateFailureInjectionTests, ArmedBoundaryFailsExactlyOnce)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Image);

		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
		EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
	}

	TEST_F(FVulkanCreateFailureInjectionTests, ArmedBoundariesRemainIndependent)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Device);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);

		EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain));
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer));
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Device));
	}

	TEST_F(FVulkanCreateFailureInjectionTests, NativeBoundaryReportsInjectedOutOfMemory)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Instance);

		try
		{
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance);
			FAIL() << "Expected the armed native creation boundary to throw.";
		}
		catch (const vk::SystemError& Error)
		{
			EXPECT_EQ(Error.code().value(), static_cast<int>(vk::Result::eErrorOutOfDeviceMemory));
		}
		EXPECT_NO_THROW(ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance));
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		InitializationFailuresRollbackAndReleaseTheBackendModule)
	{
		const std::array FailureCases = {
			std::pair{EVulkanCreateFailurePoint::Instance,
				std::string_view("Vulkan instance creation failed")},
			std::pair{EVulkanCreateFailurePoint::Device,
				std::string_view("Vulkan logical-device creation failed")},
			std::pair{EVulkanCreateFailurePoint::Allocator,
				std::string_view("Vulkan allocator creation failed")},
		};

		for (const auto& [FailurePoint, ExpectedDiagnostic] : FailureCases)
		{
			SCOPED_TRACE(ExpectedDiagnostic);
			ArmVulkanCreateFailure(FailurePoint);

			EXPECT_FALSE(RHIInit());
			EXPECT_EQ(GDynamicRHI, nullptr);
			EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
			EXPECT_NE(GetLastRHIInitializationDiagnostic().find(
				ExpectedDiagnostic), std::string_view::npos);
		}

		ASSERT_TRUE(RHIInit());
		EXPECT_TRUE(GetLastRHIInitializationDiagnostic().empty());
		EXPECT_TRUE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
		RHIExit();
		FModuleManager::Get().UnloadModule("VulkanRHI");
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		InlineRuntimeFactoryFailureReturnsNullThenRecovers)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit());
		EXPECT_EQ(GRHIThread, nullptr);

		FRHICommandListImmediate& RHICmdList =
			FRHICommandListImmediate::Get();
		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"RecoverableInlineBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
		EXPECT_FALSE(GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc));
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		Buffer = nullptr;
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread)
	{
		ASSERT_TRUE(RHIInit());
		ASSERT_NE(GRHIThread, nullptr);
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { InitRenderingThread(); }
			~FRenderingThreadScope() { ShutdownRenderingThread(); }
		} RenderingThreadScope;

		FRHICommandListImmediate& RHICmdList =
			FRHICommandListImmediate::Get();

		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"RecoverableBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
		EXPECT_FALSE(GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc));
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableTexture", 4, 4, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Flags = ETextureCreateFlags::ShaderResource;
		for (const EVulkanCreateFailurePoint FailurePoint : {
			EVulkanCreateFailurePoint::Image,
			EVulkanCreateFailurePoint::ImageView})
		{
			ArmVulkanCreateFailure(FailurePoint);
			EXPECT_FALSE(GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc));
		}
		FTextureRHIRef Texture =
			GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc);
		ASSERT_TRUE(Texture);

		FRHISamplerDesc SamplerDesc;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Sampler);
		EXPECT_FALSE(GDynamicRHI->RHICreateSampler(SamplerDesc));
		TRefCountPtr<FRHISampler> Sampler =
			GDynamicRHI->RHICreateSampler(SamplerDesc);
		ASSERT_TRUE(Sampler);

		FVertexDeclarationElementList Elements;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
		EXPECT_FALSE(GDynamicRHI->RHICreateVertexDeclaration(Elements));
		FVertexDeclarationRHIRef VertexDeclaration =
			GDynamicRHI->RHICreateVertexDeclaration(Elements);
		ASSERT_TRUE(VertexDeclaration);
		bool bRHIThreadLocalFailureReturnedNull = false;
		FVertexDeclarationRHIRef RHIThreadLocalVertexDeclaration;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			bRHIThreadLocalFailureReturnedNull =
				!GDynamicRHI->RHICreateVertexDeclaration(Elements);
			RHIThreadLocalVertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration(Elements);
		});
		EXPECT_TRUE(bRHIThreadLocalFailureReturnedNull);
		ASSERT_TRUE(RHIThreadLocalVertexDeclaration);

		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "RecoverableResourceFactories.slang";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {"VertexMain", "FragmentMain"};
		CompileOptions.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput CompileOutput =
			Compiler.Compile(ShaderPath.string(), CompileOptions);
		ASSERT_TRUE(CompileOutput) << CompileOutput.ErrorMessage;
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 2u);

		auto MakeCreateDesc = [](const FCompiledShader& CompiledShader) {
			FRHIShaderCreateDesc Desc = FRHIShaderCreateDesc::Create(
				CompiledShader.DebugName.c_str(), CompiledShader.Frequency,
				*CompiledShader.Code, CompiledShader.Hash);
			Desc.SetEntryPoint(CompiledShader.BinaryEntryPoint.c_str());
			return Desc;
		};
		const FRHIShaderCreateDesc VertexShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[0]);
		const FRHIShaderCreateDesc FragmentShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[1]);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::ShaderModule);
		EXPECT_FALSE(GDynamicRHI->RHICreateShader(VertexShaderDesc));
		FShaderRHIRef VertexShader =
			GDynamicRHI->RHICreateShader(VertexShaderDesc);
		FShaderRHIRef FragmentShader =
			GDynamicRHI->RHICreateShader(FragmentShaderDesc);
		ASSERT_TRUE(VertexShader);
		ASSERT_TRUE(FragmentShader);

		FRHIRenderTargetLayout RenderTargetLayout;
		RenderTargetLayout.NumColorRenderTargets = 1;
		auto& ColorAttachment =
			RenderTargetLayout.ColorAttachments[0].RenderTarget;
		ColorAttachment.Format = EPixelFormat::RGBA8_UNORM;
		ColorAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		ColorAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		ColorAttachment.InitialLayout = ERHITextureLayout::Undefined;
		ColorAttachment.InitialAccess = ERHIAccess::None;
		ColorAttachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		ColorAttachment.FinalAccess = ERHIAccess::ShaderRead;
		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderTargetLayout = RenderTargetLayout;
		Initializer.BoundShaders.VertexShader = VertexShader;
		Initializer.BoundShaders.FragmentShader = FragmentShader;
		Initializer.VertexDeclaration = VertexDeclaration;

		const FName PipelineName("RecoverableGraphicsPipeline");
		const FVulkanGraphicsPipelineTestStats PipelineStatsBefore =
			GetVulkanGraphicsPipelineTestStats();
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::PipelineLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::GraphicsPipeline);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		FGraphicsPipelineStateRHIRef Pipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(Pipeline);
		FGraphicsPipelineStateRHIRef SameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(SameNamePipeline);
		EXPECT_NE(Pipeline.GetReference(), SameNamePipeline.GetReference());
		EXPECT_EQ(Pipeline->GetRefCount(), 1u);
		EXPECT_EQ(SameNamePipeline->GetRefCount(), 1u);

		FGraphicsPipelineStateInitializer ChangedInitializer = Initializer;
		ChangedInitializer.RasterizerState.CullMode = ERHICullMode::None;
		FGraphicsPipelineStateRHIRef ChangedSameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, ChangedInitializer);
		ASSERT_TRUE(ChangedSameNamePipeline);
		EXPECT_NE(Pipeline.GetReference(), ChangedSameNamePipeline.GetReference());
		EXPECT_EQ(ChangedSameNamePipeline->GetRefCount(), 1u);

		std::vector<FGraphicsPipelineStateRHIRef> StatePipelines;
		auto CreateStatePipeline = [&](FGraphicsPipelineStateInitializer State,
			std::string_view Suffix) {
			FGraphicsPipelineStateRHIRef StatePipeline =
				GDynamicRHI->RHICreateGraphicsPipelineState(
					FName(std::format("RecoverableGraphicsPipeline_{}", Suffix)),
					State);
			EXPECT_TRUE(StatePipeline);
			if (StatePipeline)
				StatePipelines.push_back(std::move(StatePipeline));
		};
		FGraphicsPipelineStateInitializer WireframeInitializer = Initializer;
		WireframeInitializer.RasterizerState.PolygonMode = ERHIPolygonMode::Line;
		CreateStatePipeline(WireframeInitializer, "Wireframe");
		FGraphicsPipelineStateInitializer CounterClockwiseInitializer = Initializer;
		CounterClockwiseInitializer.RasterizerState.FrontFace =
			ERHIFrontFace::CounterClockwise;
		CreateStatePipeline(CounterClockwiseInitializer, "CounterClockwise");
		FGraphicsPipelineStateInitializer DepthInitializer = Initializer;
		DepthInitializer.DepthState.bEnableTest = true;
		DepthInitializer.DepthState.bEnableWrite = true;
		CreateStatePipeline(DepthInitializer, "Depth");
		FGraphicsPipelineStateInitializer BlendInitializer = Initializer;
		BlendInitializer.ColorBlendState = FRHIColorBlendState::StraightAlpha();
		CreateStatePipeline(BlendInitializer, "StraightAlpha");

		FGraphicsPipelineStateInitializer InvalidInitializer = Initializer;
		InvalidInitializer.RasterizerState.FrontFace = ERHIFrontFace::Count;
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			"InvalidGraphicsPipeline", InvalidInitializer));
		const FVulkanGraphicsPipelineTestStats PipelineStatsAfterCreation =
			GetVulkanGraphicsPipelineTestStats();
		EXPECT_EQ(
			PipelineStatsAfterCreation.CommittedPipelineCount,
			PipelineStatsBefore.CommittedPipelineCount + 7);
		EXPECT_EQ(
			PipelineStatsAfterCreation.CreatedPipelineLayoutCount,
			PipelineStatsBefore.CreatedPipelineLayoutCount + 8);
		EXPECT_EQ(
			PipelineStatsAfterCreation.RolledBackPipelineLayoutCount,
			PipelineStatsBefore.RolledBackPipelineLayoutCount + 1);

		StatePipelines.clear();
		ChangedSameNamePipeline = nullptr;
		SameNamePipeline = nullptr;
		Pipeline = nullptr;
		FragmentShader = nullptr;
		VertexShader = nullptr;
		RHIThreadLocalVertexDeclaration = nullptr;
		VertexDeclaration = nullptr;
		Sampler = nullptr;
		Texture = nullptr;
		Buffer = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		const FVulkanGraphicsPipelineTestStats PipelineStatsAfterRelease =
			GetVulkanGraphicsPipelineTestStats();
		EXPECT_EQ(
			PipelineStatsAfterRelease.DestroyedPipelineCount,
			PipelineStatsBefore.DestroyedPipelineCount + 7);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		ViewportOutputCandidatesFailAtomicallyAndRecover)
	{
		class FTestApplication final : public FGenericApplication
		{
		public:
			explicit FTestApplication(std::shared_ptr<FGenericWindow> InWindow)
				: Window(std::move(InWindow))
			{
			}

			auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle)
				-> std::shared_ptr<FGenericWindow> override
			{
				return Window != nullptr
					&& Window->GetOSNativeWindowHandle() == InNativeWindowHandle
					? Window
					: nullptr;
			}

			std::shared_ptr<FGenericWindow> Window;
		};

		struct FApplicationCoreScope
		{
			FApplicationCoreScope() { InitializeApplicationCore(); }
			~FApplicationCoreScope()
			{
				GApp = nullptr;
				ShutdownApplicationCore();
			}
		} ApplicationCoreScope;
		std::shared_ptr<FGenericWindow> Window = MakePlatformWindow();
		auto WindowDefinition = std::make_shared<FGenericWindowDefinition>();
		WindowDefinition->XDesiredPositionOnScreen = 0.0f;
		WindowDefinition->YDesiredPositionOnScreen = 0.0f;
		WindowDefinition->WidthDesiredOnScreen = 64.0f;
		WindowDefinition->HeightDesiredOnScreen = 64.0f;
		WindowDefinition->Title = "Vulkan viewport transaction test";
		Window->Initialize(WindowDefinition);
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
		GApp = std::make_shared<FTestApplication>(Window);
		ASSERT_EQ(GApp->FindWindowByNativeWindowHandle(
			Window->GetOSNativeWindowHandle()), Window);

		ASSERT_TRUE(RHIInit());
		ASSERT_NE(GRHIThread, nullptr);
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { InitRenderingThread(); }
			~FRenderingThreadScope() { ShutdownRenderingThread(); }
		} RenderingThreadScope;

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport(
			Window->GetOSNativeWindowHandle(), 64, 64, false,
			EPixelFormat::SBGRA8_UNORM,
			EViewportPresentModePolicy::MainWindow);
		ASSERT_TRUE(Viewport);

		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport.GetReference());
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
		});
		bool bInitialBackBufferWasNull = false;
		ENQUEUE_RENDER_COMMAND(CheckUnavailableVulkanViewport)(
			[Viewport, &bInitialBackBufferWasNull](FRHICommandListImmediate&) {
				bInitialBackBufferWasNull =
					!GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
			});
		FlushRenderingCommands();
		EXPECT_TRUE(bInitialBackBufferWasNull);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->BeginDrawing();
		});
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain));
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		vk::SwapchainKHR FirstSwapchain = VK_NULL_HANDLE;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			FirstSwapchain = VulkanViewport->GetSwapchain()->GetHandle();
		});
		ASSERT_TRUE(FirstSwapchain);

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		GDynamicRHI->RHIResizeViewport(Viewport, 80, 80, false);
		FlushRenderingCommands();
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_EQ(VulkanViewport->GetSwapchain()->GetHandle(), FirstSwapchain);
		});

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::SwapchainImageView);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_EQ(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
		});

		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_NE(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::SwapchainSemaphore);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
			VulkanViewport->RecreateSwapchain();
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});

		Viewport = nullptr;
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}
} // namespace Durin::VulkanRHI
