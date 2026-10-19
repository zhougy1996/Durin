#include "NativeRHIExecutionTestSupport.h"
#include "VulkanIntegrationTestFixture.h"
#include "VulkanPipelinePrecacheTestSupport.h"
#include "VulkanTestSupport.h"
#include "VulkanRHITestEnvironment.h"
#include "Backend/RHIValidation.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Logging/Logger.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Threading/Task.h"
#include "SlangShaderCompiler.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanRHIPrivate.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	class FVulkanResourceIntegrationTests : public FVulkanIntegrationTest
	{
	};

	TEST_F(FVulkanResourceIntegrationTests, InlineViewportGameThreadEntryDispatchesNativeWorkToRenderingThread)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		const auto Context = GetVulkanTestInitializationContext();
		ASSERT_TRUE(Context.GetPresentationTarget());
		ASSERT_TRUE(RHIInit(Context));
		if (!IsTaskSchedulerRunning()) ASSERT_TRUE(InitializeTaskScheduler(2));
		struct FRenderingScope
		{
			FRenderingScope() { InitRenderingThread(); }
			~FRenderingScope() { ShutdownRenderingThread(); }
		} RenderingScope;
		EXPECT_FALSE(IsInRHIExecutionThread());
		auto Viewport = GDynamicRHI->RHICreateViewport({
			.PresentationTarget = *Context.GetPresentationTarget(),
			.SizeX = 64, .SizeY = 64,
			.bAdoptInitializationPresentationCandidate = true});
		ASSERT_TRUE(Viewport);
		GDynamicRHI->RHIResizeViewport(Viewport, 32, 48, false);
		FIntPoint Extent;
		ENQUEUE_RENDER_COMMAND(ObserveInlineViewportResize)(
			[&](FRHICommandListImmediate& Commands) {
				CheckRHIExecutionThread();
				auto BackBuffer = Viewport->GetBackBuffer(Commands);
				ASSERT_TRUE(BackBuffer);
				Extent = {static_cast<int32>(BackBuffer->GetSizeX()),
					static_cast<int32>(BackBuffer->GetSizeY())};
			});
		FlushRenderingCommands();
		// The window system may impose its current surface extent on the request.
		EXPECT_GT(Extent.x, 0);
		EXPECT_GT(Extent.y, 0);
		Viewport = nullptr;
		FlushRenderingCommands();
	}

	TEST_F(FVulkanResourceIntegrationTests, NativeCreationFailureDiagnosticsLogOneResult)
	{
		auto& Logger = FLogger::Get();
		ASSERT_TRUE(Logger.Initialize({}));
		struct FLoggerScope { ~FLoggerScope() { FLogger::Get().Shutdown(); } } LoggerScope;
		Logger.Flush();
		const auto Cursor = Logger.ReadRecords(1, 0).NewestAvailableSequence + 1;
		LogVulkanCreationFailure(vk::Result::eErrorOutOfDeviceMemory, "test resource");
		Logger.Flush();
		const auto Logs = Logger.ReadRecords(Cursor);
		ASSERT_EQ(Logs.Records.size(), 1u);
		EXPECT_EQ(Logs.Records.front().Level, ELogLevel::Error);
	}

	TEST_F(FVulkanResourceIntegrationTests, DynamicDiscardBuffersSharePageAndPreserveExtendedPatches)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
			auto& Commands = FRHICommandListImmediate::Get();
			Commands.SwitchPipeline(ERHIPipeline::Graphics);
			auto Desc = FRHIBufferCreateDesc::CreateVertex("SharedDynamicPageTest", 256);
			Desc.Usage |= EBufferUsageFlags::Dynamic | EBufferUsageFlags::CPUWriteDiscard;
			auto First = GDynamicRHI->RHICreateBuffer(Commands, Desc);
			auto Second = GDynamicRHI->RHICreateBuffer(Commands, Desc);
			ASSERT_TRUE(First && Second);
			for (const auto& Buffer : {First, Second})
			{
				auto* Bytes = static_cast<std::byte*>(Commands.LockBuffer(Buffer, 0, 16, EResourceLockMode::WriteOnlyDiscard));
				std::memset(Bytes, 7, 16);
				Commands.UnlockBuffer(Buffer);
				// Full logical transitions must not cover neighboring page slices.
				Commands.TransitionBuffers(std::array{FRHIBufferTransition::Whole(Buffer, ERHIAccess::Discard, ERHIAccess::VertexBufferRead)});
			}
			Commands.BindVertexBuffer(0, First, 0);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				const auto* A = FVulkanBuffer::Cast(First.GetReference());
				const auto* B = FVulkanBuffer::Cast(Second.GetReference());
				EXPECT_EQ(A->GetHandle(), B->GetHandle());
				EXPECT_NE(A->GetBackingOffset(), B->GetBackingOffset());
				EXPECT_EQ(A->GetMappedSize(), 16u);
				EXPECT_EQ(A->GetSize(), 256u);
				EXPECT_EQ(A->GetNativeRangeSize(0, 256), 16u);
				EXPECT_EQ(A->GetNativeRangeSize(16, 240), 0u);
			});
			const std::array Patch{std::byte{9}};
			Commands.WriteBuffer(First, 32, Patch);
			Commands.TransitionBuffers(std::array{FRHIBufferTransition{First, 32, 1,
				ERHIAccess::TransferWrite, ERHIAccess::VertexBufferRead}});
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				const auto* A = FVulkanBuffer::Cast(First.GetReference());
				const auto* B = FVulkanBuffer::Cast(Second.GetReference());
				EXPECT_NE(A->GetHandle(), B->GetHandle());
				EXPECT_EQ(A->GetMappedSize(), 256u);
				const auto* Bytes = static_cast<const std::byte*>(A->GetMappedPointer());
				EXPECT_EQ(Bytes[0], std::byte{7});
				EXPECT_EQ(Bytes[15], std::byte{7});
				EXPECT_EQ(Bytes[32], std::byte{9});
				ERHIAccess Tracked = ERHIAccess::None;
				EXPECT_TRUE(A->GetStateTracker().Validate(0, 16, ERHIAccess::VertexBufferRead, Tracked));
				EXPECT_EQ(static_cast<const std::byte*>(B->GetMappedPointer())[0], std::byte{7});
			});
			Commands.BindVertexBuffer(0, nullptr, 0);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		});
	}

	TEST_F(FVulkanResourceIntegrationTests, NativeResourceFactoriesRunOutsideReplayAndReleaseResources)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR) / "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		for (const char* Mode : {"threaded", "inline"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
				auto& Commands = FRHICommandListImmediate::Get();
				const auto BufferDesc = FRHIBufferCreateDesc::Create("IndependentBuffer", 256, 0, EBufferUsageFlags::FormattedBuffer);
				const auto TextureDesc = FRHITextureCreateDesc::Create2D("IndependentTexture", 4, 4, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::ShaderResource);
				auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, BufferDesc);
				auto Texture = GDynamicRHI->RHICreateTexture(Commands, TextureDesc);
				ASSERT_TRUE(Buffer); ASSERT_TRUE(Texture);
				const auto BufferViewDesc = MakeDefaultBufferViewDesc(*Buffer, ERHIBufferViewType::Formatted, EPixelFormat::R32_UINT);
				const auto TextureViewDesc = MakeDefaultTextureViewDesc(*Texture, ERHITextureViewUsage::Sampled);
				const auto& Shader = Compiled.CompiledShaders[0];
				auto ShaderDesc = FRHIShaderCreateDesc::Create("IndependentShader", Shader.Frequency, Shader.Code, Shader.CodeHash);
				ShaderDesc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
				const std::array<std::function<TRefCountPtr<FRHIResource>()>, 6> Factories{
					[&] { return GDynamicRHI->RHICreateShader(ShaderDesc); },
					[&] { return GDynamicRHI->RHICreateSampler({}); },
					[&] { return GDynamicRHI->RHICreateBuffer(Commands, BufferDesc); },
					[&] { return GDynamicRHI->RHICreateTexture(Commands, TextureDesc); },
					[&] { return GDynamicRHI->RHICreateBufferView(Buffer, BufferViewDesc); },
					[&] { return GDynamicRHI->RHICreateTextureView(Texture, TextureViewDesc); }};
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
				const auto BeforeMemory = GDynamicRHI->RHIGetMemoryStatistics();
				std::promise<void> Entered, Release;
				auto EnteredFuture = Entered.get_future();
				auto Released = Release.get_future().share();
				if (GRHIThread)
				{
					Commands.EnqueueLambda([&] { Entered.set_value(); Released.wait(); }, 0);
					GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
					EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				}
				const auto Before = GCommandListExecutor.GetStats();
				std::vector<std::future<bool>> Workers;
				for (uint32 Index = 0; Index < 16; ++Index)
					Workers.push_back(std::async(std::launch::async, [&] {
						bool Complete = true;
						for (const auto& Factory : Factories) { auto Resource = Factory(); Complete = !!Resource && Complete; }
						return Complete;
					}));
				for (auto& Worker : Workers) EXPECT_EQ(Worker.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				Release.set_value();
				for (auto& Worker : Workers) EXPECT_TRUE(Worker.get());
				const auto After = GCommandListExecutor.GetStats();
				EXPECT_EQ(After.SynchronousOperationCount, Before.SynchronousOperationCount);
				EXPECT_EQ(After.LastSubmittedSerial, Before.LastSubmittedSerial);
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
				GDynamicRHI->RHIBlockUntilGPUIdle();
				GCommandListExecutor.ExecuteSynchronousOperation(false, [] {
					ReleaseCompletedVulkanResourcesForTesting();
				});
				const auto AfterMemory = GDynamicRHI->RHIGetMemoryStatistics();
				for (size_t Index = 0; Index < BeforeMemory.Classes.size(); ++Index)
				{
					EXPECT_EQ(AfterMemory.Classes[Index].LiveAllocationCount, BeforeMemory.Classes[Index].LiveAllocationCount);
					EXPECT_EQ(AfterMemory.Classes[Index].LiveBytes, BeforeMemory.Classes[Index].LiveBytes);
				}
				Buffer = nullptr; Texture = nullptr;
			});
			RHIExit();
		}
	}

	TEST_F(FVulkanResourceIntegrationTests, VertexDeclarationsDoNotDependOnReplay)
	{
		for (const char* Mode : {"inline", "threaded"})
		{
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
				std::promise<void> ReleaseReplay;
				auto Released = ReleaseReplay.get_future().share();
				FRHICommandListFence HeldFence;
				if (GRHIThread)
				{
					auto Started = std::make_shared<std::promise<void>>();
					auto StartedFuture = Started->get_future();
					GCommandListExecutor.GetImmediateCommandList().EnqueueLambda([Started, Released] {
						Started->set_value();
						Released.wait();
					}, 0);
					GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
					HeldFence = GCommandListExecutor.CreateFence();
					EXPECT_EQ(StartedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				}
				const auto Before = GCommandListExecutor.GetStats();
				const auto BeforeSerial = GCommandListExecutor.GetLastSubmittedSerial();
				FVertexDeclarationElementList Elements{};
				Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 12);
				auto Pending = std::async(std::launch::async, [&] {
					return GDynamicRHI->RHICreateVertexDeclaration(Elements);
				});
				// A regression may enqueue; release replay before get() so failure cannot deadlock teardown.
				EXPECT_EQ(Pending.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				ReleaseReplay.set_value();
				auto Declaration = Pending.get();
				if (GRHIThread) HeldFence.Wait();
				ASSERT_TRUE(Declaration);
				Elements[0].Offset = 4;
				EXPECT_EQ(Declaration->GetElements()[0].Offset, 0);
				EXPECT_EQ(Declaration->GetElements()[0].Stride, 12);
				auto Updated = GDynamicRHI->RHICreateVertexDeclaration(Elements);
				ASSERT_TRUE(Updated);
				EXPECT_EQ(Updated->GetElements()[0].Offset, 4);
				const auto After = GCommandListExecutor.GetStats();
				EXPECT_EQ(After.SynchronousOperationCount, Before.SynchronousOperationCount);
				EXPECT_EQ(GCommandListExecutor.GetLastSubmittedSerial(), BeforeSerial);
				Updated = nullptr;
				Declaration = nullptr;
			});
			RHIExit();
		}
	}

	TEST_F(FVulkanResourceIntegrationTests,
		CapabilitiesAndExactTextureSupportRejectBeforeNativeCreation)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		::Durin::Testing::RunOnRHIRenderingThreadForTests([&] {
			const FRHICapabilities* Capabilities = GDynamicRHI->RHIGetCapabilities();
			ASSERT_NE(Capabilities, nullptr);
			EXPECT_EQ(Capabilities->FeatureLevel, ERHIFeatureLevel::ES3_1);
			EXPECT_EQ(Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::Texture2D
					| ERHITextureDimensionFlags::Texture2DArray
					| ERHITextureDimensionFlags::Texture3D
					| ERHITextureDimensionFlags::TextureCube);
			EXPECT_GE(Capabilities->MaxTextureDimension2D, 1u);
			EXPECT_GE(Capabilities->MaxTextureDimension3D, 1u);
			EXPECT_GE(Capabilities->MaxTextureDimensionCube, 1u);
			EXPECT_GE(Capabilities->MaxTextureArrayLayers,
				static_cast<uint32>(TextureCubeFaceCount));
			EXPECT_TRUE(std::ranges::all_of(
				Capabilities->MaxComputeWorkGroupCount,
				[](uint32 Limit) { return Limit > 0; }));
			EXPECT_TRUE(EnumHasAnyFlags(Capabilities->ColorSampleCounts,
				ERHISampleCountFlags::Samples1));
			EXPECT_TRUE(EnumHasAnyFlags(Capabilities->DepthSampleCounts,
				ERHISampleCountFlags::Samples1));
			EXPECT_TRUE(Capabilities->bSupportsGPUTimestamps);
			EXPECT_GT(Capabilities->GPUTimestampNanosecondsPerTick, 0.0);

			FRHITextureCreateDesc Texture2D = FRHITextureCreateDesc::Create2D(
				"Supported2D", 4, 4, EPixelFormat::RGBA8_UNORM);
			Texture2D.Flags = ETextureCreateFlags::ShaderResource;
			FRHITextureCreateDesc TextureCube = FRHITextureCreateDesc::CreateCube(
				"SupportedCube").SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FRHITextureCreateDesc Texture2DArray =
				FRHITextureCreateDesc::Create2DArray("Supported2DArray")
					.SetExtent(4, 4).SetArraySize(3)
					.SetFormat(EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::ShaderResource);
			FRHITextureCreateDesc Texture3D = FRHITextureCreateDesc::Create3D(
				"Supported3D").SetExtent(4, 4).SetDepth(4)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture2D));
			EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture2DArray));
			EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture3D));
			EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(TextureCube));
			for (EPixelFormat Format : {EPixelFormat::R8_UNORM, EPixelFormat::RG8_UNORM,
				EPixelFormat::RGBA8_UNORM, EPixelFormat::R16_FLOAT,
				EPixelFormat::RGBA16_FLOAT})
			{
				Texture3D.SetFormat(Format).SetFlags(ETextureCreateFlags::ShaderResource);
				EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture3D))
					<< static_cast<uint32>(Format);
			}
			for (EPixelFormat Format : {EPixelFormat::R8_UNORM, EPixelFormat::RGBA16_FLOAT})
			{
				Texture3D.SetFormat(Format).SetFlags(ETextureCreateFlags::Storage);
				EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture3D))
					<< static_cast<uint32>(Format);
			}

			const std::array DeferredDescriptions{
				FRHITextureCreateDesc::CreateCubeArray("DeferredCubeArray")
					.SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM),
			};
			FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
			for (const FRHITextureCreateDesc& Desc : DeferredDescriptions)
			{
				SCOPED_TRACE(static_cast<uint32>(Desc.Dimension));
				EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Desc));
				EXPECT_FALSE(GDynamicRHI->RHICreateTexture(RHICmdList, Desc));
			}

			FRHITextureCreateDesc Oversized = Texture2D;
			Oversized.SetExtent(static_cast<int32>(Capabilities->MaxTextureDimension2D + 1), 1);
			const auto TextureCreateDescResult = ValidateTextureCreateDesc(Oversized);
			ASSERT_TRUE(TextureCreateDescResult) << ToString(TextureCreateDescResult.error());
			EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Oversized));

			FTextureRHIRef Created2D = GDynamicRHI->RHICreateTexture(RHICmdList, Texture2D);
			FTextureRHIRef Created2DArray =
				GDynamicRHI->RHICreateTexture(RHICmdList, Texture2DArray);
			Texture3D.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Created3D = GDynamicRHI->RHICreateTexture(RHICmdList, Texture3D);
			FTextureRHIRef CreatedCube = GDynamicRHI->RHICreateTexture(RHICmdList, TextureCube);
			EXPECT_TRUE(Created2D);
			EXPECT_TRUE(Created2DArray);
			EXPECT_TRUE(Created3D);
			EXPECT_TRUE(CreatedCube);
			Created2D = nullptr;
			Created2DArray = nullptr;
			Created3D = nullptr;
			CreatedCube = nullptr;
			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		});
	}

	TEST_F(FVulkanResourceIntegrationTests,
		RuntimeFactoriesCreateAndReuseResourcesOnTheSameRHIThread)
	{
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		ResetVulkanMemoryBaselineStatistics();
		if (!IsTaskSchedulerRunning()) ASSERT_TRUE(InitializeTaskScheduler(2));
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
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableTexture", 4, 4, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Flags = ETextureCreateFlags::ShaderResource;
		FTextureRHIRef Texture =
			GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc);
		ASSERT_TRUE(Texture);
		const FRHITextureViewDesc TextureViewDesc = MakeDefaultTextureViewDesc(
			*Texture, ERHITextureViewUsage::Sampled);
		FTextureViewRHIRef TextureView = GDynamicRHI->RHICreateTextureView(
			Texture, TextureViewDesc);
		ASSERT_TRUE(TextureView);
		RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
			Texture, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead}});
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);

		FRHISamplerDesc SamplerDesc;
		TRefCountPtr<FRHISampler> Sampler =
			GDynamicRHI->RHICreateSampler(SamplerDesc);
		ASSERT_TRUE(Sampler);

		FVertexDeclarationElementList Elements;
		FVertexDeclarationRHIRef VertexDeclaration =
			GDynamicRHI->RHICreateVertexDeclaration(Elements);
		ASSERT_TRUE(VertexDeclaration);
		FVertexDeclarationRHIRef RHIThreadLocalVertexDeclaration;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			RHIThreadLocalVertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration(Elements);
		});
		ASSERT_TRUE(RHIThreadLocalVertexDeclaration);

		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "RecoverableResourceFactories.slang";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {
			"VertexMain", "FragmentMain", "FragmentMrtMain"};
		CompileOptions.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment,
			EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput CompileOutput =
			Compiler.Compile(ShaderPath.string(), CompileOptions);
		ASSERT_TRUE(CompileOutput) << FormatShaderError(CompileOutput.Error);
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 3u);

		auto MakeCreateDesc = [](const FShaderCode& CompiledShader) {
			FRHIShaderCreateDesc Desc = FRHIShaderCreateDesc::Create(
				CompiledShader.DebugName.c_str(), CompiledShader.Frequency,
				CompiledShader.Code, CompiledShader.CodeHash);
			Desc.SetEntryPoint(CompiledShader.BinaryEntryPoint.c_str());
			return Desc;
		};
		const FRHIShaderCreateDesc VertexShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[0]);
		const FRHIShaderCreateDesc FragmentShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[1]);
		const FRHIShaderCreateDesc MrtFragmentShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[2]);
		FShaderRHIRef VertexShader =
			GDynamicRHI->RHICreateShader(VertexShaderDesc);
		FShaderRHIRef FragmentShader =
			GDynamicRHI->RHICreateShader(FragmentShaderDesc);
		FShaderRHIRef MrtFragmentShader =
			GDynamicRHI->RHICreateShader(MrtFragmentShaderDesc);
		ASSERT_TRUE(VertexShader);
		ASSERT_TRUE(FragmentShader);
		ASSERT_TRUE(MrtFragmentShader);

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
		ColorAttachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderTargetLayout = RenderTargetLayout;
		Initializer.BoundShaders.VertexShader = VertexShader;
		Initializer.BoundShaders.FragmentShader = FragmentShader;
		Initializer.VertexDeclaration = VertexDeclaration;
		auto& PipelineBindings =
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts;
		PipelineBindings.emplace_back(
			EShaderStageFlags::Vertex, 0, ERHIBindingType::Texture);
		PipelineBindings.emplace_back(
			EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler, 2);

		const FName PipelineName("RecoverableGraphicsPipeline");
		const FRHIPipelineCacheStatistics CacheStatsBefore =
			GDynamicRHI->RHIGetPipelineCacheStatistics();
		FGraphicsPipelineStateRHIRef Pipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(Pipeline);
		FGraphicsPipelineStateRHIRef SameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(SameNamePipeline);
		EXPECT_EQ(Pipeline.GetReference(), SameNamePipeline.GetReference());

		FGraphicsPipelineStateInitializer ChangedInitializer = Initializer;
		ChangedInitializer.RasterizerState.CullMode = ERHICullMode::None;
		FGraphicsPipelineStateRHIRef ChangedSameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, ChangedInitializer);
		ASSERT_TRUE(ChangedSameNamePipeline);
		EXPECT_NE(Pipeline.GetReference(), ChangedSameNamePipeline.GetReference());

		FGraphicsPipelineStateInitializer PositiveZeroInitializer = Initializer;
		PositiveZeroInitializer.RasterizerState.bEnableDepthBias = true;
		PositiveZeroInitializer.RasterizerState.DepthBiasConstantFactor = 0.0f;
		PositiveZeroInitializer.RasterizerState.DepthBiasClamp = 0.0f;
		PositiveZeroInitializer.RasterizerState.DepthBiasSlopeFactor = 0.0f;
		FGraphicsPipelineStateInitializer NegativeZeroInitializer =
			PositiveZeroInitializer;
		NegativeZeroInitializer.RasterizerState.DepthBiasConstantFactor = -0.0f;
		NegativeZeroInitializer.RasterizerState.DepthBiasClamp = -0.0f;
		NegativeZeroInitializer.RasterizerState.DepthBiasSlopeFactor = -0.0f;
		FGraphicsPipelineStateRHIRef PositiveZeroPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_PositiveZero", PositiveZeroInitializer);
		FGraphicsPipelineStateRHIRef NegativeZeroPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_NegativeZero", NegativeZeroInitializer);
		ASSERT_TRUE(PositiveZeroPipeline && NegativeZeroPipeline);
		EXPECT_EQ(PositiveZeroPipeline.GetReference(),
			NegativeZeroPipeline.GetReference());
		FGraphicsPipelineStateInitializer NonzeroBiasInitializer =
			PositiveZeroInitializer;
		NonzeroBiasInitializer.RasterizerState.DepthBiasConstantFactor = 1.25f;
		NonzeroBiasInitializer.RasterizerState.DepthBiasClamp = 4.0f;
		NonzeroBiasInitializer.RasterizerState.DepthBiasSlopeFactor = 1.75f;
		FGraphicsPipelineStateRHIRef NonzeroBiasPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_NonzeroBias", NonzeroBiasInitializer);
		ASSERT_TRUE(NonzeroBiasPipeline);
		EXPECT_EQ(PositiveZeroPipeline.GetReference(),
			NonzeroBiasPipeline.GetReference());

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
		DepthInitializer.RenderTargetLayout.bHasDepthStencil = true;
		auto& DepthAttachment =
			DepthInitializer.RenderTargetLayout.DepthStencilAttachment;
		DepthAttachment.Format = EPixelFormat::D32;
		DepthAttachment.InitialLayout = ERHITextureLayout::Undefined;
		DepthAttachment.InitialAccess = ERHIAccess::None;
		DepthAttachment.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
		DepthAttachment.FinalAccess = ERHIAccess::DepthStencilReadWrite;
		DepthInitializer.DepthStencilState.bEnableTest = true;
		DepthInitializer.DepthStencilState.bEnableWrite = true;
		CreateStatePipeline(DepthInitializer, "Depth");
		FGraphicsPipelineStateInitializer BlendInitializer = Initializer;
		BlendInitializer.ColorBlendStates[0] = FRHIColorBlendState::StraightAlpha();
		CreateStatePipeline(BlendInitializer, "StraightAlpha");
		FRHITextureCreateDesc MrtDepthDesc = FRHITextureCreateDesc::Create2D(
			"MrtStencilValidationDepth", 8, 8, EPixelFormat::D24S8)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
		if (!GDynamicRHI->RHIIsTextureSupported(MrtDepthDesc))
		{
			MrtDepthDesc.Format = EPixelFormat::D32S8;
		}
		ASSERT_TRUE(GDynamicRHI->RHIIsTextureSupported(MrtDepthDesc));
		FGraphicsPipelineStateInitializer MrtStencilInitializer = Initializer;
		MrtStencilInitializer.RenderTargetLayout.NumColorRenderTargets = 2;
		MrtStencilInitializer.RenderTargetLayout.ColorAttachments[1] =
			MrtStencilInitializer.RenderTargetLayout.ColorAttachments[0];
		MrtStencilInitializer.RenderTargetLayout.bHasDepthStencil = true;
		auto& MrtDepthAttachment =
			MrtStencilInitializer.RenderTargetLayout.DepthStencilAttachment;
		MrtDepthAttachment.Format = MrtDepthDesc.Format;
		MrtDepthAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		MrtDepthAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		MrtDepthAttachment.StencilLoadAction = ERHIRenderTargetLoadAction::Clear;
		MrtDepthAttachment.StencilStoreAction = ERHIRenderTargetStoreAction::Store;
		MrtDepthAttachment.InitialLayout = ERHITextureLayout::Undefined;
		MrtDepthAttachment.InitialAccess = ERHIAccess::None;
		MrtDepthAttachment.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
		MrtDepthAttachment.FinalAccess = ERHIAccess::DepthStencilReadWrite;
		MrtStencilInitializer.DepthStencilState.bEnableTest = true;
		MrtStencilInitializer.DepthStencilState.bEnableWrite = true;
		MrtStencilInitializer.DepthStencilState.bEnableStencil = true;
		MrtStencilInitializer.ColorBlendStates[1] =
			FRHIColorBlendState::StraightAlpha();
		MrtStencilInitializer.BoundShaders.FragmentShader = MrtFragmentShader;
		FGraphicsPipelineStateRHIRef MrtStencilPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_MrtStencil", MrtStencilInitializer);
		ASSERT_TRUE(MrtStencilPipeline);
		StatePipelines.push_back(MrtStencilPipeline);

		FGraphicsPipelineStateInitializer TwoSetInitializer = Initializer;
		TwoSetInitializer.PipelineLayout.BindingLayouts.emplace_back()
			.BindingLayouts.emplace_back(
				EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler);
		FGraphicsPipelineStateRHIRef TwoSetPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoveredDependentDescriptor", TwoSetInitializer);
		ASSERT_TRUE(TwoSetPipeline);

		FGraphicsPipelineStateInitializer InvalidInitializer = Initializer;
		InvalidInitializer.RasterizerState.FrontFace = ERHIFrontFace::Count;
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			"InvalidGraphicsPipeline", InvalidInitializer));
		const FRHIPipelineCacheStatistics CacheStatsAfterCreation =
			GDynamicRHI->RHIGetPipelineCacheStatistics();
		EXPECT_GE(CacheStatsAfterCreation.GraphicsPipelines.Hits,
			CacheStatsBefore.GraphicsPipelines.Hits + 1);
		EXPECT_EQ(CacheStatsAfterCreation.GraphicsPipelines.NativeCreations,
			CacheStatsBefore.GraphicsPipelines.NativeCreations + 9);
		EXPECT_EQ(CacheStatsAfterCreation.GraphicsPipelines.Occupancy,
			CacheStatsBefore.GraphicsPipelines.Occupancy + 9);
		EXPECT_EQ(CacheStatsAfterCreation.GraphicsPipelines.Capacity, 2048u);
		EXPECT_EQ(CacheStatsAfterCreation.StructuralLayouts.Capacity, 256u);
		EXPECT_EQ(CacheStatsAfterCreation.DescriptorSnapshots.Capacity, 512u);
		EXPECT_EQ(CacheStatsAfterCreation.DescriptorValueCapacity, 8192u);

		FRHITextureCreateDesc RenderTargetDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableFramebufferTexture", 8, 8, EPixelFormat::RGBA8_UNORM);
		RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable
			| ETextureCreateFlags::ShaderResource;
		FTextureRHIRef RenderTarget =
			GDynamicRHI->RHICreateTexture(RHICmdList, RenderTargetDesc);
		ASSERT_TRUE(RenderTarget);
		FRHIRenderPassInfo PassInfo;
		PassInfo.RenderTargetLayout = RenderTargetLayout;
		PassInfo.ColorRenderTargets[0] = RenderTarget;
		PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			auto* Context = GDynamicRHI->RHIGetDefaultContext();
			Context->RHIBeginRenderPass(PassInfo, "RecoveredFramebuffer");
			Context->RHIEndRenderPass();
		});
		auto DrawDescriptorArray = [&](bool& bFailed) {
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				auto* Context = GDynamicRHI->RHIGetDefaultContext();
				Context->RHIBeginRenderPass(PassInfo, "DescriptorArrayDraw");
				Context->RHISetGraphicsPipelineState(*Pipeline);
				Context->RHISetViewport(0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 1.0f);
				FTextureViewRHIRef TransientTextureView =
					GDynamicRHI->RHICreateTextureView(Texture,
						MakeDefaultTextureViewDesc(*Texture,
							ERHITextureViewUsage::Sampled));
				check(TransientTextureView);
				std::array<FRHIShaderParameterResource, 1> TextureParameters{
					FRHIShaderParameterResource{.Resource = TransientTextureView.GetReference(),
						.SetIndex = 0, .BindingIndex = 0, .ArrayElement = 0,
						.Type = ERHIBindingType::Texture}};
				Context->RHISetShaderParameters(VertexShader, TextureParameters);
				TransientTextureView = nullptr;
				std::array<FRHIShaderParameterResource, 2> SamplerParameters{
					FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
						.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 0,
						.Type = ERHIBindingType::Sampler},
					FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
						.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 1,
						.Type = ERHIBindingType::Sampler}};
				Context->RHISetShaderParameters(FragmentShader, SamplerParameters);
				try
				{
					Context->RHIDraw({.VertexCount = 3});
				}
				catch (...)
				{
					Context->RHIEndRenderPass();
					bFailed = true;
					return;
				}
				Context->RHIEndRenderPass();
			});
		};
		bool bDescriptorDrawFailed = false;
		DrawDescriptorArray(bDescriptorDrawFailed);
		EXPECT_FALSE(bDescriptorDrawFailed);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			SubmitAndRetireDescriptorPoolsForTesting();
		});
		const FVulkanMemoryBaselineStatistics DescriptorBaseline =
			GetVulkanMemoryBaselineStatistics();
		EXPECT_GT(DescriptorBaseline.DescriptorPoolCount, 0u);
		EXPECT_GT(DescriptorBaseline.DescriptorPoolSetCapacity, 0u);
		EXPECT_GT(DescriptorBaseline.DescriptorPeakAllocatedSetCount, 0u);
		FRHITextureCreateDesc MrtColorDesc = FRHITextureCreateDesc::Create2D(
			"MrtStencilValidationColor", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::CPUReadback);
		FTextureRHIRef MrtColor0 =
			GDynamicRHI->RHICreateTexture(RHICmdList, MrtColorDesc);
		MrtColorDesc.DebugName = "MrtStencilValidationColor1";
		FTextureRHIRef MrtColor1 =
			GDynamicRHI->RHICreateTexture(RHICmdList, MrtColorDesc);
		FTextureRHIRef MrtDepth =
			GDynamicRHI->RHICreateTexture(RHICmdList, MrtDepthDesc);
		ASSERT_TRUE(MrtColor0 && MrtColor1 && MrtDepth);
		FRHIRenderPassInfo MrtPassInfo;
		MrtPassInfo.RenderTargetLayout = MrtStencilInitializer.RenderTargetLayout;
		MrtPassInfo.ColorRenderTargets[0] = MrtColor0;
		MrtPassInfo.ColorRenderTargets[1] = MrtColor1;
		MrtPassInfo.DepthStencilRenderTarget = MrtDepth;
		MrtPassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		MrtPassInfo.ColorClearValues[1] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		Durin::FByteBuffer MrtPixels;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			auto* Context = GDynamicRHI->RHIGetDefaultContext();
			Context->RHIBeginRenderPass(MrtPassInfo, "MrtStencilDraw");
			Context->RHISetViewport(0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 1.0f);
			Context->RHISetGraphicsPipelineState(*MrtStencilPipeline);
			std::array<FRHIShaderParameterResource, 3> Parameters{
				FRHIShaderParameterResource{.Resource = TextureView.GetReference(),
					.SetIndex = 0, .BindingIndex = 0, .ArrayElement = 0,
					.Type = ERHIBindingType::Texture},
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 0,
					.Type = ERHIBindingType::Sampler},
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 1,
					.Type = ERHIBindingType::Sampler}};
			Context->RHISetShaderParameters(VertexShader,
				std::span(Parameters).first(1));
			Context->RHISetShaderParameters(MrtFragmentShader,
				std::span(Parameters).subspan(1));
			Context->RHIDraw({.VertexCount = 3});
			Context->RHIEndRenderPass();
		});
		ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
			RHICmdList, MrtColor0, 0, 0, MrtPixels));
		EXPECT_EQ(MrtPixels.size(), 8u * 8u * 4u);

		StatePipelines.clear();
		PositiveZeroPipeline = nullptr;
		NegativeZeroPipeline = nullptr;
		TwoSetPipeline = nullptr;
		ChangedSameNamePipeline = nullptr;
		SameNamePipeline = nullptr;
		Pipeline = nullptr;
		FragmentShader = nullptr;
		MrtFragmentShader = nullptr;
		VertexShader = nullptr;
		RHIThreadLocalVertexDeclaration = nullptr;
		VertexDeclaration = nullptr;
		Sampler = nullptr;
		TextureView = nullptr;
		Texture = nullptr;
		RenderTarget = nullptr;
		MrtDepth = nullptr;
		MrtColor1 = nullptr;
		MrtColor0 = nullptr;
		Buffer = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		GDynamicRHI->RHIResetPipelineCacheStatistics();
		const FRHIPipelineCacheStatistics ResetCacheStats =
			GDynamicRHI->RHIGetPipelineCacheStatistics();
		EXPECT_EQ(ResetCacheStats.GraphicsPipelines.Hits, 0u);
		EXPECT_EQ(ResetCacheStats.GraphicsPipelines.NativeCreations, 0u);
		EXPECT_EQ(ResetCacheStats.GraphicsPipelines.Occupancy,
			CacheStatsAfterCreation.GraphicsPipelines.Occupancy);
	}
}
