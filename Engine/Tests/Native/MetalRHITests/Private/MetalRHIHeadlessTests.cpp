#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#if DURIN_WITH_EDITOR
#include "Shader/Shader.h"
#include "Shader/IShaderBuildModule.h"
#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#endif

#include <gtest/gtest.h>

namespace
{
	class FScopedEnvironmentVariable
	{
	public:
		FScopedEnvironmentVariable(const char* InName, const char* Value)
			: Name(InName)
		{
			if (const char* Previous = std::getenv(InName)) Original = Previous;
			setenv(InName, Value, 1);
		}

		~FScopedEnvironmentVariable()
		{
			if (Original) setenv(Name, Original->c_str(), 1);
			else unsetenv(Name);
		}
	private:
		const char* Name;
		std::optional<std::string> Original;
	};

	struct FScopedRHIExit
	{
		~FScopedRHIExit()
		{
			if (Durin::GDynamicRHI) Durin::RHIExit();
		}
	};
}

#if DURIN_WITH_EDITOR
TEST(FMetalRHIHeadlessTests, AuthoredImGuiShaderBuildMatchesVulkanThroughProductionRHI)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::FModuleManager::Get().LoadModule("ShaderBuild"));
	auto* Builder = Durin::IShaderBuildModule::Get();
	ASSERT_NE(Builder, nullptr);
	Durin::FShaderCompileOptions Options;
	Options.Target = Durin::MetalShaderTarget;
	Options.EntryPoints = {"VertexMain", "FragmentMain"};
	Options.Frequencies = {Durin::EShaderFrequency::Vertex,
		Durin::EShaderFrequency::Fragment};
	const auto MetalCompiled = Builder->CompileMounted("/Engine/ImGui", Options);
	ASSERT_TRUE(MetalCompiled) << Durin::FormatShaderError(MetalCompiled.Error);
	ASSERT_EQ(MetalCompiled.CompiledShaders.size(), 2u);
	Options.Target = Durin::VulkanShaderTarget;
	const auto VulkanCompiled = Builder->CompileMounted("/Engine/ImGui", Options);
	ASSERT_TRUE(VulkanCompiled) << Durin::FormatShaderError(VulkanCompiled.Error);
	ASSERT_EQ(VulkanCompiled.CompiledShaders.size(), 2u);
	std::array<Durin::FByteBuffer, 2> MetalPixels;
	for (const char* BackendName : {"metal", "vulkan"})
	{
		FScopedEnvironmentVariable SelectedBackend("DURIN_RHI_BACKEND", BackendName);
		const auto& Compiled = std::string_view(BackendName) == "metal"
			? MetalCompiled : VulkanCompiled;
		for (size_t ModeIndex = 0; ModeIndex < 2; ++ModeIndex)
		{
			const char* Mode = ModeIndex == 0 ? "inline" : "threaded";
			SCOPED_TRACE(BackendName);
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Vertex = Durin::GDynamicRHI->RHICreateShader(
				Durin::MakeShaderCreateDesc(Compiled.CompiledShaders[0]));
			auto Fragment = Durin::GDynamicRHI->RHICreateShader(
				Durin::MakeShaderCreateDesc(Compiled.CompiledShaders[1]));
			ASSERT_TRUE(Vertex);
			ASSERT_TRUE(Fragment);
			struct FImGuiVertex
			{
				float Position[2];
				float UV[2];
				float Color[4];
			};
			const FImGuiVertex Vertices[3] = {
				{{-1, -1}, {0.5f, 0.5f}, {0.5f, 1, 1, 0.5f}},
				{{3, -1}, {0.5f, 0.5f}, {0.5f, 1, 1, 0.5f}},
				{{-1, 3}, {0.5f, 0.5f}, {0.5f, 1, 1, 0.5f}}};
			auto VertexDesc = Durin::FRHIBufferCreateDesc::Create(
				"Authored ImGui vertices", sizeof(Vertices), sizeof(FImGuiVertex),
				Durin::EBufferUsageFlags::VertexBuffer);
			VertexDesc.InitialData = {.Data = Vertices, .Size = sizeof(Vertices)};
			auto VertexResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, VertexDesc);
			ASSERT_TRUE(VertexResult.has_value());
			auto VertexBuffer = std::move(*VertexResult);
			Durin::FVertexDeclarationElementList Elements{};
			Elements[0] = {0, 0, Durin::EVertexElementType::Float2, 0,
				sizeof(FImGuiVertex)};
			Elements[1] = {0, 8, Durin::EVertexElementType::Float2, 1,
				sizeof(FImGuiVertex)};
			Elements[2] = {0, 16, Durin::EVertexElementType::Float4, 2,
				sizeof(FImGuiVertex)};
			auto Declaration = Durin::GDynamicRHI->RHICreateVertexDeclaration(Elements);
			ASSERT_TRUE(Declaration);
			const std::array<uint8_t, 4> FontPixel{128, 64, 255, 255};
			auto FontDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Authored ImGui font", 1, 1, Durin::EPixelFormat::RGBA8_UNORM);
			FontDesc.SetFlags(Durin::ETextureCreateFlags::ShaderResource);
			auto FontResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, FontDesc);
			ASSERT_TRUE(FontResult.has_value());
			auto Font = std::move(*FontResult);
			Durin::GDynamicRHI->RHIUpdateTexture2D(Commands, Font.GetReference(),
				0, 0, Durin::FUpdateTextureRegion2D(0, 0, 0, 0, 1, 1), 4,
				std::as_bytes(std::span(FontPixel)));
			auto FontView = Durin::GDynamicRHI->RHICreateTextureView(
				Font.GetReference(), Durin::MakeDefaultTextureViewDesc(
					*Font, Durin::ERHITextureViewUsage::Sampled));
			auto Sampler = Durin::GDynamicRHI->RHICreateSampler(
				Durin::FRHISamplerDesc::PointClamp());
			ASSERT_TRUE(FontView);
			ASSERT_TRUE(Sampler);
			const float Projection[4] = {1, 1, 0, 0};
			auto UniformDesc = Durin::FRHIBufferCreateDesc::Create(
				"Authored ImGui projection", sizeof(Projection), sizeof(Projection),
				Durin::EBufferUsageFlags::UniformBuffer);
			UniformDesc.InitialData = {.Data = Projection, .Size = sizeof(Projection)};
			auto UniformResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, UniformDesc);
			ASSERT_TRUE(UniformResult.has_value());
			auto Uniform = std::move(*UniformResult);
			auto UniformView = Durin::GDynamicRHI->RHICreateBufferView(
				Uniform.GetReference(), {.Offset = 0, .Size = sizeof(Projection),
					.Type = Durin::ERHIBufferViewType::Uniform});
			ASSERT_TRUE(UniformView);
			auto ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Authored ImGui target", 4, 4, Durin::EPixelFormat::RGBA8_UNORM);
			ColorDesc.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
				| Durin::ETextureCreateFlags::CPUReadback);
			auto ColorResult = Durin::GDynamicRHI->RHITryCreateTexture(
				Commands, ColorDesc);
			ASSERT_TRUE(ColorResult.has_value());
			auto Color = std::move(*ColorResult);
			Durin::FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout.NumColorRenderTargets = 1;
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
				Durin::EPixelFormat::RGBA8_UNORM;
			Pass.ColorRenderTargets[0] = Color.GetReference();
			Pass.ColorClearValues[0] = Durin::FClearValueBinding(0, 0, 1, 1);
			Durin::FGraphicsPipelineStateInitializer Initializer;
			Initializer.BoundShaders = {Vertex.GetReference(), Fragment.GetReference()};
			Initializer.VertexDeclaration = Declaration.GetReference();
			Initializer.RenderTargetLayout = Pass.RenderTargetLayout;
			Initializer.RasterizerState.CullMode = Durin::ERHICullMode::None;
			Initializer.ColorBlendStates[0] = Durin::FRHIColorBlendState::StraightAlpha();
			ASSERT_TRUE(Durin::BuildPipelineLayoutFromShaders(
				Compiled.CompiledShaders, Initializer.PipelineLayout));
			auto Pipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
				"AuthoredImGui", Initializer);
			ASSERT_TRUE(Pipeline);
			const std::array VertexParameters{Durin::FRHIShaderParameterResource{
				.Resource = UniformView.GetReference(), .SetIndex = 0,
				.BindingIndex = 2, .Type = Durin::ERHIBindingType::UniformBuffer}};
			const std::array FragmentParameters{
				Durin::FRHIShaderParameterResource{.Resource = FontView.GetReference(),
					.SetIndex = 0, .BindingIndex = 0,
					.Type = Durin::ERHIBindingType::Texture},
				Durin::FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1,
					.Type = Durin::ERHIBindingType::Sampler}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.BeginRenderPass(Pass, "AuthoredImGui");
			Commands.SetGraphicsPipelineState(*Pipeline);
			Commands.SetViewport(0, 0, 0, 4, 4, 1);
			Commands.SetScissor(1, 1, 2, 2);
			Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
			Commands.SetShaderParameters(Vertex.GetReference(), VertexParameters);
			Commands.SetShaderParameters(Fragment.GetReference(), FragmentParameters);
			Commands.Draw({.VertexCount = 3});
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				Signal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			Durin::FByteBuffer Pixels;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Color.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 4u * 4u * 4u);
			for (size_t Y = 0; Y < 4; ++Y)
			for (size_t X = 0; X < 4; ++X)
			{
				const size_t Index = (Y * 4 + X) * 4;
				const bool bClipped = X < 1 || X >= 3 || Y < 1 || Y >= 3;
				const int Expected = bClipped ? 0 : 32;
				EXPECT_LE(std::abs(int(Pixels[Index]) - Expected), 1);
				EXPECT_LE(std::abs(int(Pixels[Index + 1]) - Expected), 1);
				EXPECT_EQ(Pixels[Index + 2], std::byte{255});
				EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
			if (std::string_view(BackendName) == "metal")
				MetalPixels[ModeIndex] = Pixels;
			else
			{
				ASSERT_EQ(Pixels.size(), MetalPixels[ModeIndex].size());
				for (size_t Index = 0; Index < Pixels.size(); ++Index)
					EXPECT_LE(std::abs(int(Pixels[Index])
						- int(MetalPixels[ModeIndex][Index])), 1);
			}
		}
	}
}

TEST(FMetalRHIHeadlessTests, AuthoredHitProxyIdsMatchVulkanThroughProductionRHI)
{
	FScopedEnvironmentVariable DefaultBackend("DURIN_RHI_BACKEND", "metal");
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::FModuleManager::Get().LoadModule("ShaderBuild"));
	auto* Builder = Durin::IShaderBuildModule::Get();
	ASSERT_NE(Builder, nullptr);
	Durin::FShaderCompileOptions Options;
	Options.Target = Durin::MetalShaderTarget;
	Options.EntryPoints = {"VertexMain", "FragmentMain"};
	Options.Frequencies = {Durin::EShaderFrequency::Vertex,
		Durin::EShaderFrequency::Fragment};
	const auto MetalCompiled = Builder->CompileMounted("/Engine/HitProxyOverlay", Options);
	ASSERT_TRUE(MetalCompiled) << Durin::FormatShaderError(MetalCompiled.Error);
	ASSERT_EQ(MetalCompiled.CompiledShaders.size(), 2u);
	Options.Target = Durin::VulkanShaderTarget;
	const auto VulkanCompiled = Builder->CompileMounted("/Engine/HitProxyOverlay", Options);
	ASSERT_TRUE(VulkanCompiled) << Durin::FormatShaderError(VulkanCompiled.Error);
	ASSERT_EQ(VulkanCompiled.CompiledShaders.size(), 2u);
	std::array<Durin::FByteBuffer, 2> MetalPixels;
	for (const char* BackendName : {"metal", "vulkan"})
	{
		FScopedEnvironmentVariable SelectedBackend("DURIN_RHI_BACKEND", BackendName);
		const auto& Compiled = std::string_view(BackendName) == "metal"
			? MetalCompiled : VulkanCompiled;
		for (size_t ModeIndex = 0; ModeIndex < 2; ++ModeIndex)
		{
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION",
				ModeIndex == 0 ? "inline" : "threaded");
			SCOPED_TRACE(BackendName);
			SCOPED_TRACE(ModeIndex);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Vertex = Durin::GDynamicRHI->RHICreateShader(
				Durin::MakeShaderCreateDesc(Compiled.CompiledShaders[0]));
			auto Fragment = Durin::GDynamicRHI->RHICreateShader(
				Durin::MakeShaderCreateDesc(Compiled.CompiledShaders[1]));
			ASSERT_TRUE(Vertex);
			ASSERT_TRUE(Fragment);
			struct FHitVertex { float Position[4]; float Distance; };
			const FHitVertex Vertices[3] = {
				{{-1, -1, 0, 1}, 0.25f},
				{{3, -1, 0, 1}, 0.25f},
				{{-1, 3, 0, 1}, 0.25f}};
			auto VertexDesc = Durin::FRHIBufferCreateDesc::Create(
				"Authored hit-proxy vertices", sizeof(Vertices), sizeof(FHitVertex),
				Durin::EBufferUsageFlags::VertexBuffer);
			VertexDesc.InitialData = {.Data = Vertices, .Size = sizeof(Vertices)};
			auto VertexResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, VertexDesc);
			ASSERT_TRUE(VertexResult.has_value());
			auto VertexBuffer = std::move(*VertexResult);
			Durin::FVertexDeclarationElementList Elements{};
			Elements[0] = {0, 0, Durin::EVertexElementType::Float4, 0,
				sizeof(FHitVertex)};
			Elements[1] = {0, 16, Durin::EVertexElementType::Float1, 1,
				sizeof(FHitVertex)};
			auto Declaration = Durin::GDynamicRHI->RHICreateVertexDeclaration(Elements);
			ASSERT_TRUE(Declaration);
			const uint32_t HitId[4] = {0x12345678u, 0, 0, 0};
			auto UniformDesc = Durin::FRHIBufferCreateDesc::Create(
				"Authored hit-proxy ID", sizeof(HitId), sizeof(HitId),
				Durin::EBufferUsageFlags::UniformBuffer);
			UniformDesc.InitialData = {.Data = HitId, .Size = sizeof(HitId)};
			auto UniformResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, UniformDesc);
			ASSERT_TRUE(UniformResult.has_value());
			auto Uniform = std::move(*UniformResult);
			auto UniformView = Durin::GDynamicRHI->RHICreateBufferView(
				Uniform.GetReference(), {.Offset = 0, .Size = sizeof(HitId),
					.Type = Durin::ERHIBufferViewType::Uniform});
			ASSERT_TRUE(UniformView);
			auto ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Authored hit-proxy target", 4, 4, Durin::EPixelFormat::RG32_UINT);
			ColorDesc.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
				| Durin::ETextureCreateFlags::CPUReadback);
			auto ColorResult = Durin::GDynamicRHI->RHITryCreateTexture(
				Commands, ColorDesc);
			ASSERT_TRUE(ColorResult.has_value());
			auto Color = std::move(*ColorResult);
			Durin::FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout.NumColorRenderTargets = 1;
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
				Durin::EPixelFormat::RG32_UINT;
			Pass.ColorRenderTargets[0] = Color.GetReference();
			Pass.ColorClearValues[0] = Durin::FClearValueBinding(0, 0, 0, 0);
			Durin::FGraphicsPipelineStateInitializer Initializer;
			Initializer.BoundShaders = {Vertex.GetReference(), Fragment.GetReference()};
			Initializer.VertexDeclaration = Declaration.GetReference();
			Initializer.RenderTargetLayout = Pass.RenderTargetLayout;
			Initializer.RasterizerState.CullMode = Durin::ERHICullMode::None;
			ASSERT_TRUE(Durin::BuildPipelineLayoutFromShaders(
				Compiled.CompiledShaders, Initializer.PipelineLayout));
			auto Pipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
				"AuthoredHitProxy", Initializer);
			ASSERT_TRUE(Pipeline);
			const std::array Parameters{Durin::FRHIShaderParameterResource{
				.Resource = UniformView.GetReference(), .SetIndex = 0,
				.BindingIndex = 0, .Type = Durin::ERHIBindingType::UniformBuffer}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.BeginRenderPass(Pass, "AuthoredHitProxy");
			Commands.SetGraphicsPipelineState(*Pipeline);
			Commands.SetViewport(0, 0, 0, 4, 4, 1);
			Commands.SetScissor(1, 1, 2, 2);
			Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
			Commands.SetShaderParameters(Fragment.GetReference(), Parameters);
			Commands.Draw({.VertexCount = 3});
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				Signal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			Durin::FByteBuffer Pixels;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Color.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 4u * 4u * 2u * sizeof(uint32_t));
			for (size_t Y = 0; Y < 4; ++Y)
			for (size_t X = 0; X < 4; ++X)
			{
				const size_t Offset = (Y * 4 + X) * 2u * sizeof(uint32_t);
				uint32_t Id = 0, Distance = 0;
				std::memcpy(&Id, Pixels.data() + Offset, sizeof(Id));
				std::memcpy(&Distance, Pixels.data() + Offset + sizeof(Id),
					sizeof(Distance));
				const bool bCovered = X >= 1 && X < 3 && Y >= 1 && Y < 3;
				EXPECT_EQ(Id, bCovered ? HitId[0] : 0u);
				EXPECT_EQ(Distance, bCovered ? 0x3e800000u : 0u);
			}
			if (std::string_view(BackendName) == "metal")
				MetalPixels[ModeIndex] = Pixels;
			else EXPECT_EQ(Pixels, MetalPixels[ModeIndex]);
		}
	}
}
#endif

TEST(FMetalRHIHeadlessTests, DeviceAndSingleQueueInitializeInBothExecutionModes)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		ASSERT_NE(Durin::GDynamicRHI, nullptr);
		const auto* Capabilities = Durin::GDynamicRHI->RHIGetCapabilities();
		ASSERT_NE(Capabilities, nullptr);
		EXPECT_EQ(Capabilities->MaxColorAttachments, 4u);
		EXPECT_TRUE(Capabilities->bSupportsIndirectDraw);
		EXPECT_TRUE(Capabilities->bSupportsIndirectDispatch);
		const auto& Queues = Durin::GDynamicRHI->RHIGetQueueCapabilities();
		ASSERT_EQ(Queues.Queues.size(), 1u);
		EXPECT_EQ(Queues.Graphics, Queues.Compute);
		EXPECT_FALSE(Queues.bIndependentCompute);
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		const auto First = Commands.BeginGPUSubmission({.Queue = Queues.Graphics});
		Commands.EndGPUSubmission();
		const auto Second = Commands.BeginGPUSubmission(
			{.Queue = Queues.Graphics, .Waits = {First}});
		Commands.EndGPUSubmission();
		EXPECT_EQ(First.GetState(), Durin::ERHIGPUSubmissionState::Pending);
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
		EXPECT_EQ(First.GetState(), Durin::ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(Second.GetState(), Durin::ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Second, 0),
			Durin::ERHIGPUWaitResult::Pending);
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Second, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		EXPECT_EQ(First.GetState(), Durin::ERHIGPUSubmissionState::Complete);
		EXPECT_TRUE(First.IsRetirementEligible());
		EXPECT_TRUE(Second.IsRetirementEligible());
		Durin::RHIExit();
		EXPECT_EQ(Durin::GDynamicRHI, nullptr);
	}
}

TEST(FMetalRHIHeadlessTests, ShutdownCancelsReplayedButUnsubmittedWork)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", "inline");
	ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
	FScopedRHIExit Exit;
	auto& Commands = Durin::FRHICommandListImmediate::Get();
	const auto Signal = Commands.BeginGPUSubmission(
		{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
	Commands.EndGPUSubmission();
	Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
	EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Pending);
	Durin::RHIExit();
	EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Canceled);
	EXPECT_TRUE(Signal.IsRetirementEligible());
}

TEST(FMetalRHIHeadlessTests, FrameEndSubmitsPendingGPUWorkInBothExecutionModes)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		for (uint32_t Frame = 0; Frame < 3; ++Frame)
		{
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::BeginFrame);
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::EndFrame);
			EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
		}
		Durin::RHIExit();
	}
}

TEST(FMetalRHIHeadlessTests, ShaderCreationValidatesTargetEntryAndNativeBindingMap)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", "inline");
	ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
	FScopedRHIExit Exit;
	const std::string Source = "#include <metal_stdlib>\n"
		"using namespace metal;\n"
		"vertex float4 vertexMain(uint vertexID [[vertex_id]]) { return float4(0, 0, 0, 1); }\n"
		"fragment float4 fragmentMain() { return float4(1, 0, 0, 1); }\n"
		"kernel void computeMain(device uint* result [[buffer(0)]], uint id [[thread_position_in_grid]]) { result[id] = id; }\n";
	Durin::FByteBuffer Code;
	for (char Character : Source) Code.push_back(std::byte(Character));
	for (const auto [Frequency, Entry] : {
		std::pair{Durin::EShaderFrequency::Vertex, "vertexMain"},
		std::pair{Durin::EShaderFrequency::Fragment, "fragmentMain"},
		std::pair{Durin::EShaderFrequency::Compute, "computeMain"}})
	{
		SCOPED_TRACE(Entry);
		auto Desc = Durin::FRHIShaderCreateDesc::Create(
			Entry, Frequency, Code, Durin::FXxHash128::HashBuffer(Code));
		Desc.Target = Durin::MetalShaderTarget;
		Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
		if (Frequency == Durin::EShaderFrequency::Compute)
			Desc.ComputeThreadGroupSize = {1, 1, 1};
		Desc.SetEntryPoint(Entry);
		if (Frequency == Durin::EShaderFrequency::Compute)
			Desc.MetalBindings.push_back({0, 0,
				Durin::ERHIBindingType::StorageBuffer, 0, 1});
		Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
			Frequency, Desc.MetalBindings, Desc.MetalPushConstantBufferSlot);
		const auto Shader = Durin::GDynamicRHI->RHICreateShader(Desc);
		ASSERT_TRUE(Shader);
		EXPECT_EQ(Shader->GetTarget(), Durin::MetalShaderTarget);
		EXPECT_EQ(Shader->GetEntryPoint(), Entry);
		EXPECT_EQ(Shader->GetMetalBindings().size(), Desc.MetalBindings.size());
		if (Frequency == Durin::EShaderFrequency::Compute)
		{
			Durin::FComputePipelineStateInitializer Pipeline;
			Pipeline.ComputeShader = Shader.GetReference();
			Pipeline.PipelineLayout.BindingLayouts.resize(1);
			Pipeline.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 0,
				Durin::ERHIBindingType::StorageBuffer);
			EXPECT_TRUE(Durin::GDynamicRHI->RHICreateComputePipelineState(
				"MetalCompute", Pipeline));
			Pipeline.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Type =
				Durin::ERHIBindingType::UniformBuffer;
			EXPECT_FALSE(Durin::GDynamicRHI->RHICreateComputePipelineState(
				"MetalComputeBadLayout", Pipeline));
		}
		Desc.BindingRemapIdentity.HashLow ^= 1;
		EXPECT_FALSE(Durin::GDynamicRHI->RHICreateShader(Desc));
		Desc.BindingRemapIdentity.HashLow ^= 1;
		if (!Desc.MetalBindings.empty())
		{
			Desc.MetalBindings.front().Slot = 1;
			Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Frequency, Desc.MetalBindings, Desc.MetalPushConstantBufferSlot);
			EXPECT_FALSE(Durin::GDynamicRHI->RHICreateShader(Desc));
			Desc.MetalBindings.front().Slot = 0;
			Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Frequency, Desc.MetalBindings, Desc.MetalPushConstantBufferSlot);
		}
		Desc.CodeFormat = Durin::EShaderCodeFormat::Spirv15;
		EXPECT_FALSE(Durin::GDynamicRHI->RHICreateShader(Desc));
		Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
		Desc.Target = Durin::VulkanShaderTarget;
		EXPECT_FALSE(Durin::GDynamicRHI->RHICreateShader(Desc));
		Desc.Target = Durin::MetalShaderTarget;
		Desc.SetEntryPoint("missingEntry");
		EXPECT_FALSE(Durin::GDynamicRHI->RHICreateShader(Desc));
	}
	Durin::RHIExit();
}

TEST(FMetalRHIHeadlessTests, RecordedTriangleDrawFillsColorTargetInBothExecutionModes)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
			"struct VertexInput { float2 position [[attribute(0)]]; };\n"
			"vertex float4 vertexMain(VertexInput input [[stage_in]]) { "
			"return float4(input.position, 0, 1); }\n"
			"fragment float4 fragmentMain() { return float4(0, 1, 0, 1); }\n"
			"fragment float4 opaqueRed() { return float4(1, 0, 0, 1); }\n"
			"fragment uint2 idsMain() { return uint2(0x12345678u, 0x9abcdef0u); }\n"
			"fragment float4 blendMain() { return float4(1, 0, 0, 0.5); }\n"
			"struct MRTOutput { float4 a [[color(0)]]; float4 b [[color(1)]]; "
			"float4 c [[color(2)]]; float4 d [[color(3)]]; };\n"
			"fragment MRTOutput mrtMain() { MRTOutput o; "
			"o.a=float4(1,0,0,1); o.b=float4(0,1,0,1); "
			"o.c=float4(0,0,1,1); o.d=float4(1,0,0,1); return o; }\n"
			"kernel void writeArgs(device uint* args [[buffer(0)]]) { "
			"args[0]=3; args[1]=1; args[2]=1; args[3]=0; args[4]=0; }\n";
		Durin::FByteBuffer Code;
		for (char Character : Source) Code.push_back(std::byte(Character));
		auto MakeShader = [&](const char* Entry,
			Durin::EShaderFrequency Frequency) {
			auto Desc = Durin::FRHIShaderCreateDesc::Create(Entry,
				Frequency, Code, Durin::FXxHash128::HashBuffer(Code));
			Desc.Target = Durin::MetalShaderTarget;
			Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			Desc.SetEntryPoint(Entry);
			if (Frequency == Durin::EShaderFrequency::Compute)
			{
				Desc.ComputeThreadGroupSize = {1, 1, 1};
				Desc.MetalBindings.push_back({0, 0,
					Durin::ERHIBindingType::StorageBuffer, 0, 1});
			}
			Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Frequency, Desc.MetalBindings,
				Desc.MetalPushConstantBufferSlot);
			return Durin::GDynamicRHI->RHICreateShader(Desc);
		};
		auto Vertex = MakeShader("vertexMain", Durin::EShaderFrequency::Vertex);
		auto Fragment = MakeShader("fragmentMain", Durin::EShaderFrequency::Fragment);
		ASSERT_TRUE(Vertex);
		ASSERT_TRUE(Fragment);
		Durin::FVertexDeclarationElementList Elements{};
		Elements[0] = {0, 0, Durin::EVertexElementType::Float2, 0, 8};
		auto Declaration = Durin::GDynamicRHI->RHICreateVertexDeclaration(Elements);
		ASSERT_TRUE(Declaration);
		const float Positions[6] = {-1.0f, -1.0f, 3.0f, -1.0f,
			-1.0f, 3.0f};
		auto VertexBufferDesc = Durin::FRHIBufferCreateDesc::Create(
			"Metal triangle vertices", sizeof(Positions), sizeof(float) * 2,
			Durin::EBufferUsageFlags::VertexBuffer);
		VertexBufferDesc.InitialData = {.Data = Positions,
			.Size = sizeof(Positions)};
		auto VertexBufferResult = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, VertexBufferDesc);
		ASSERT_TRUE(VertexBufferResult.has_value());
		auto VertexBuffer = std::move(*VertexBufferResult);
		// Exercise an upload recorded before any explicit GPU submission.
		Commands.WriteBuffer(VertexBuffer.GetReference(), Positions,
			sizeof(Positions), 0);
		auto TextureDesc = Durin::FRHITextureCreateDesc::Create2D(
			"Metal triangle target", 8, 8, Durin::EPixelFormat::RGBA8_UNORM);
		TextureDesc.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
			| Durin::ETextureCreateFlags::CPUReadback);
		auto Created = Durin::GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
		ASSERT_TRUE(Created.has_value());
		auto Texture = std::move(*Created);
		Durin::FRHIRenderPassInfo Pass;
		Pass.RenderTargetLayout.NumColorRenderTargets = 1;
		Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
			Durin::EPixelFormat::RGBA8_UNORM;
		Pass.ColorRenderTargets[0] = Texture.GetReference();
		Pass.ColorClearValues[0] = Durin::FClearValueBinding(1.0f, 0.0f, 0.0f, 1.0f);
		ASSERT_TRUE(Pass.RenderTargetLayout.IsValid());
		Durin::FGraphicsPipelineStateInitializer Initializer;
		Initializer.BoundShaders = {Vertex.GetReference(), Fragment.GetReference()};
		Initializer.VertexDeclaration = Declaration.GetReference();
		Initializer.RenderTargetLayout = Pass.RenderTargetLayout;
		Initializer.RasterizerState.CullMode = Durin::ERHICullMode::None;
		auto Pipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalTriangle", Initializer);
		ASSERT_TRUE(Pipeline);
		const auto Signal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Commands.BeginRenderPass(Pass, "MetalTriangle");
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.SetGraphicsPipelineState(*Pipeline);
		Commands.Draw({.VertexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		Durin::FByteBuffer Pixels;
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		const float LinePositions[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
		auto LineBufferDesc = Durin::FRHIBufferCreateDesc::Create(
			"Metal line vertices", sizeof(LinePositions), sizeof(float) * 2,
			Durin::EBufferUsageFlags::VertexBuffer);
		LineBufferDesc.InitialData = {.Data = LinePositions,
			.Size = sizeof(LinePositions)};
		auto LineBufferResult = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, LineBufferDesc);
		ASSERT_TRUE(LineBufferResult.has_value());
		auto LineBuffer = std::move(*LineBufferResult);
		auto LineInitializer = Initializer;
		LineInitializer.PrimitiveTopology =
			Durin::FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList;
		auto LinePipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalLine", LineInitializer);
		ASSERT_TRUE(LinePipeline);
		const auto LineSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(Pass, "MetalLine");
		Commands.SetGraphicsPipelineState(*LinePipeline);
		Commands.BindVertexBuffer(0, LineBuffer.GetReference(), 0);
		Commands.Draw({.VertexCount = 2});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			LineSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		size_t GreenPixels = 0;
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			if (Pixels[Index] == std::byte{0}
				&& Pixels[Index + 1] == std::byte{255}) ++GreenPixels;
		EXPECT_GT(GreenPixels, 0u);
		EXPECT_LT(GreenPixels, 64u);
		const auto ClippedSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(Pass, "MetalClippedTriangle");
		Commands.SetGraphicsPipelineState(*Pipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.SetViewport(0, 0, 0, 4, 8, 1);
		Commands.SetScissor(1, 2, 2, 4);
		Commands.Draw({.VertexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			ClippedSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (uint32_t Y = 0; Y < 8; ++Y)
			for (uint32_t X = 0; X < 8; ++X)
			{
				const size_t Offset = (Y * 8 + X) * 4;
				const bool bDrawn = X >= 1 && X < 3 && Y >= 2 && Y < 6;
				EXPECT_EQ(Pixels[Offset], bDrawn ? std::byte{0} : std::byte{255});
				EXPECT_EQ(Pixels[Offset + 1], bDrawn ? std::byte{255} : std::byte{0});
				EXPECT_EQ(Pixels[Offset + 2], std::byte{0});
				EXPECT_EQ(Pixels[Offset + 3], std::byte{255});
			}
		const uint16_t Indices[4] = {9, 0, 1, 2};
		auto IndexDesc = Durin::FRHIBufferCreateDesc::Create(
			"Metal triangle indices", sizeof(Indices), sizeof(uint16_t),
			Durin::EBufferUsageFlags::IndexBuffer);
		IndexDesc.InitialData = {.Data = Indices, .Size = sizeof(Indices)};
		auto IndexResult = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, IndexDesc);
		ASSERT_TRUE(IndexResult.has_value());
		auto IndexBuffer = std::move(*IndexResult);
		const auto IndexedSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(Pass, "MetalIndexedTriangle");
		Commands.SetGraphicsPipelineState(*Pipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.BindIndexBuffer(IndexBuffer.GetReference(), sizeof(uint16_t));
		Commands.DrawIndexed({.IndexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			IndexedSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		ASSERT_NE(Durin::GDynamicRHI->RHIGetCapabilities(), nullptr);
		EXPECT_TRUE(Durin::GDynamicRHI->RHIGetCapabilities()->bSupportsIndirectDraw);
		const Durin::FRHIDrawIndirectArguments DrawArgs{3, 1, 0, 0};
		auto DrawArgsDesc = Durin::FRHIBufferCreateDesc::Create(
			"Metal indirect draw arguments", sizeof(DrawArgs), sizeof(uint32_t),
			Durin::EBufferUsageFlags::DrawIndirect);
		DrawArgsDesc.InitialData = {.Data = &DrawArgs, .Size = sizeof(DrawArgs)};
		auto DrawArgsResult = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, DrawArgsDesc);
		ASSERT_TRUE(DrawArgsResult.has_value());
		auto DrawArgsBuffer = std::move(*DrawArgsResult);
		const auto IndirectSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(Pass, "MetalIndirectTriangle");
		Commands.SetGraphicsPipelineState(*Pipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.DrawIndirect(DrawArgsBuffer.GetReference(), 0);
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			IndirectSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		const Durin::FRHIDrawIndexedIndirectArguments IndexedArgs{3, 1, 1, 0, 0};
		auto IndexedArgsDesc = Durin::FRHIBufferCreateDesc::Create(
			"Metal indexed indirect arguments", sizeof(IndexedArgs),
			sizeof(uint32_t), Durin::EBufferUsageFlags::DrawIndirect);
		IndexedArgsDesc.InitialData = {.Data = &IndexedArgs,
			.Size = sizeof(IndexedArgs)};
		auto IndexedArgsResult = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, IndexedArgsDesc);
		ASSERT_TRUE(IndexedArgsResult.has_value());
		auto IndexedArgsBuffer = std::move(*IndexedArgsResult);
		const auto IndexedIndirectSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(Pass, "MetalIndexedIndirectTriangle");
		Commands.SetGraphicsPipelineState(*Pipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.BindIndexBuffer(IndexBuffer.GetReference(), sizeof(uint16_t));
		Commands.DrawIndexedIndirect(IndexedArgsBuffer.GetReference(), 0);
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			IndexedIndirectSignal, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		auto ArgsShader = MakeShader("writeArgs", Durin::EShaderFrequency::Compute);
		ASSERT_TRUE(ArgsShader);
		Durin::FComputePipelineStateInitializer ArgsPipelineDesc;
		ArgsPipelineDesc.ComputeShader = ArgsShader.GetReference();
		ArgsPipelineDesc.PipelineLayout.BindingLayouts.resize(1);
		ArgsPipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
			Durin::EShaderStageFlags::Compute, 0,
			Durin::ERHIBindingType::StorageBuffer);
		auto ArgsPipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
			"MetalWriteIndirectArgs", ArgsPipelineDesc);
		ASSERT_TRUE(ArgsPipeline);
		auto GpuArgsDesc = Durin::FRHIBufferCreateDesc::Create(
			"Metal GPU authored indexed draw", sizeof(IndexedArgs),
			sizeof(uint32_t), Durin::EBufferUsageFlags::DrawIndirect
				| Durin::EBufferUsageFlags::StructuredBuffer
				| Durin::EBufferUsageFlags::UnorderedAccess);
		auto GpuArgsResult = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, GpuArgsDesc);
		ASSERT_TRUE(GpuArgsResult.has_value());
		auto GpuArgs = std::move(*GpuArgsResult);
		const Durin::FRHIBufferViewDesc GpuArgsViewDesc{
			.Offset = 0, .Size = sizeof(IndexedArgs),
			.Type = Durin::ERHIBufferViewType::StructuredStorage};
		auto GpuArgsView = Durin::GDynamicRHI->RHICreateBufferView(
			GpuArgs.GetReference(), GpuArgsViewDesc);
		ASSERT_TRUE(GpuArgsView);
		const Durin::FRHIShaderParameterResource GpuArgsParameter{
			.Resource = GpuArgsView.GetReference(), .SetIndex = 0,
			.BindingIndex = 0, .Type = Durin::ERHIBindingType::StorageBuffer};
		const auto GpuIndirectSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
		Commands.SetComputePipelineState(*ArgsPipeline);
		Commands.SetShaderParameters(ArgsShader.GetReference(),
			std::span(&GpuArgsParameter, 1));
		Commands.Dispatch(1, 1, 1);
		Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Commands.BeginRenderPass(Pass, "MetalGpuAuthoredIndirectTriangle");
		Commands.SetGraphicsPipelineState(*Pipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.BindIndexBuffer(IndexBuffer.GetReference(), sizeof(uint16_t));
		Commands.DrawIndexedIndirect(GpuArgs.GetReference(), 0);
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			GpuIndirectSignal, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		auto BlendFragment = MakeShader("blendMain", Durin::EShaderFrequency::Fragment);
		ASSERT_TRUE(BlendFragment);
		auto BlendInitializer = Initializer;
		BlendInitializer.BoundShaders.FragmentShader = BlendFragment.GetReference();
		BlendInitializer.ColorBlendStates[0] = Durin::FRHIColorBlendState::StraightAlpha();
		auto BlendPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalAlphaBlend", BlendInitializer);
		ASSERT_TRUE(BlendPipeline);
		auto BlendPass = Pass;
		BlendPass.ColorClearValues[0] = Durin::FClearValueBinding(0.0f, 0.0f, 1.0f, 1.0f);
		const auto BlendSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(BlendPass, "MetalAlphaBlend");
		Commands.SetGraphicsPipelineState(*BlendPipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.Draw({.VertexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			BlendSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{128});
			EXPECT_EQ(Pixels[Index + 1], std::byte{0});
			EXPECT_EQ(Pixels[Index + 2], std::byte{128});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		auto CheckBlendFactors = [&](const char* Name,
			const Durin::FRHIColorBlendState& BlendState,
			std::byte Red, std::byte Blue, std::byte Alpha) {
			auto BlendFactorInitializer = BlendInitializer;
			BlendFactorInitializer.ColorBlendStates[0] = BlendState;
			auto Pipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
				Name, BlendFactorInitializer);
			EXPECT_TRUE(Pipeline);
			if (!Pipeline) return;
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.BeginRenderPass(BlendPass, Name);
			Commands.SetGraphicsPipelineState(*Pipeline);
			Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
			Commands.Draw({.VertexCount = 3});
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				Signal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			Pixels.clear();
			EXPECT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Texture.GetReference(), 0, 0, Pixels));
			EXPECT_EQ(Pixels.size(), 8u * 8u * 4u);
			for (size_t Index = 0; Index + 3 < Pixels.size(); Index += 4)
			{
				EXPECT_EQ(Pixels[Index], Red);
				EXPECT_EQ(Pixels[Index + 1], std::byte{0});
				EXPECT_EQ(Pixels[Index + 2], Blue);
				EXPECT_EQ(Pixels[Index + 3], Alpha);
			}
		};
		auto ConstantBlend = BlendInitializer.ColorBlendStates[0];
		ConstantBlend.SrcColorFactor = Durin::ERHIBlendFactor::OneMinusConstantColor;
		ConstantBlend.DstColorFactor = Durin::ERHIBlendFactor::ConstantColor;
		ConstantBlend.SrcAlphaFactor = Durin::ERHIBlendFactor::OneMinusConstantAlpha;
		ConstantBlend.DstAlphaFactor = Durin::ERHIBlendFactor::ConstantAlpha;
		CheckBlendFactors("MetalConstantBlend", ConstantBlend,
			std::byte{255}, std::byte{0}, std::byte{128});
		auto SaturatedBlend = BlendInitializer.ColorBlendStates[0];
		SaturatedBlend.SrcColorFactor = Durin::ERHIBlendFactor::SrcAlphaSaturate;
		SaturatedBlend.DstColorFactor = Durin::ERHIBlendFactor::One;
		CheckBlendFactors("MetalSaturatedBlend", SaturatedBlend,
			std::byte{0}, std::byte{255}, std::byte{255});
		auto MaskInitializer = BlendInitializer;
		MaskInitializer.ColorBlendStates[0] = {};
		MaskInitializer.ColorBlendStates[0].ColorWriteMask =
			Durin::ERHIColorWriteMask::Red;
		auto MaskPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalRedOnly", MaskInitializer);
		ASSERT_TRUE(MaskPipeline);
		const auto MaskSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(BlendPass, "MetalRedOnly");
		Commands.SetGraphicsPipelineState(*MaskPipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.Draw({.VertexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			MaskSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Texture.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{255});
			EXPECT_EQ(Pixels[Index + 1], std::byte{0});
			EXPECT_EQ(Pixels[Index + 2], std::byte{255});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		auto MrtFragment = MakeShader("mrtMain", Durin::EShaderFrequency::Fragment);
		ASSERT_TRUE(MrtFragment);
		auto SecondTextureResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, TextureDesc);
		auto ThirdTextureResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, TextureDesc);
		auto EmissiveDesc = TextureDesc;
		EmissiveDesc.Format = Durin::EPixelFormat::R11G11B10_FLOAT;
		auto EmissiveResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, EmissiveDesc);
		auto GBufferDepthDesc = Durin::FRHITextureCreateDesc::Create2D(
			"Metal GBuffer depth", 8, 8, Durin::EPixelFormat::D32);
		GBufferDepthDesc.SetFlags(Durin::ETextureCreateFlags::DepthStencilTargetable
			| Durin::ETextureCreateFlags::CPUReadback);
		auto GBufferDepthResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, GBufferDepthDesc);
		ASSERT_TRUE(SecondTextureResult.has_value());
		ASSERT_TRUE(ThirdTextureResult.has_value());
		ASSERT_TRUE(EmissiveResult.has_value());
		ASSERT_TRUE(GBufferDepthResult.has_value());
		auto SecondTexture = std::move(*SecondTextureResult);
		auto ThirdTexture = std::move(*ThirdTextureResult);
		auto Emissive = std::move(*EmissiveResult);
		auto GBufferDepth = std::move(*GBufferDepthResult);
		auto MrtPass = Pass;
		MrtPass.RenderTargetLayout.NumColorRenderTargets = 4;
		for (uint32_t Index = 1; Index < 4; ++Index)
			MrtPass.RenderTargetLayout.ColorAttachments[Index] =
				MrtPass.RenderTargetLayout.ColorAttachments[0];
		MrtPass.RenderTargetLayout.ColorAttachments[3].RenderTarget.Format =
			Durin::EPixelFormat::R11G11B10_FLOAT;
		MrtPass.ColorRenderTargets[1] = SecondTexture.GetReference();
		MrtPass.ColorRenderTargets[2] = ThirdTexture.GetReference();
		MrtPass.ColorRenderTargets[3] = Emissive.GetReference();
		MrtPass.ColorClearValues[1] = Durin::FClearValueBinding(0.0f, 0.0f, 1.0f, 1.0f);
		MrtPass.ColorClearValues[2] = Durin::FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		MrtPass.ColorClearValues[3] = Durin::FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		MrtPass.RenderTargetLayout.bHasDepthStencil = true;
		MrtPass.RenderTargetLayout.DepthStencilAttachment.Format =
			Durin::EPixelFormat::D32;
		MrtPass.DepthStencilRenderTarget = GBufferDepth.GetReference();
		MrtPass.DepthStencilClearValue = Durin::FClearValueBinding(1.0f, 0u);
		ASSERT_TRUE(MrtPass.RenderTargetLayout.IsValid());
		auto MrtInitializer = Initializer;
		MrtInitializer.BoundShaders.FragmentShader = MrtFragment.GetReference();
		MrtInitializer.RenderTargetLayout = MrtPass.RenderTargetLayout;
		MrtInitializer.DepthStencilState.bEnableTest = true;
		MrtInitializer.DepthStencilState.bEnableWrite = true;
		MrtInitializer.DepthStencilState.CompareOp = Durin::ERHIDepthCompareOp::Less;
		auto MrtPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalFourColorTargets", MrtInitializer);
		ASSERT_TRUE(MrtPipeline);
		const auto MrtSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.BeginRenderPass(MrtPass, "MetalFourColorTargets");
		Commands.SetGraphicsPipelineState(*MrtPipeline);
		Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
		Commands.Draw({.VertexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			MrtSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		for (auto* Target : {Texture.GetReference(), SecondTexture.GetReference(),
			ThirdTexture.GetReference()})
		{
			Pixels.clear();
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Target, 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
			const bool bFirst = Target == Texture.GetReference();
			const bool bThird = Target == ThirdTexture.GetReference();
			for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			{
				EXPECT_EQ(Pixels[Index], bFirst ? std::byte{255} : std::byte{0});
				EXPECT_EQ(Pixels[Index + 1], (!bFirst && !bThird)
					? std::byte{255} : std::byte{0});
				EXPECT_EQ(Pixels[Index + 2], bThird ? std::byte{255} : std::byte{0});
				EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
		}
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Emissive.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0xc0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{0x03});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{0});
		}
		Pixels.clear();
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, GBufferDepth.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * sizeof(float));
		for (size_t Index = 0; Index < Pixels.size(); Index += sizeof(float))
		{
			float Depth = 1.0f;
			std::memcpy(&Depth, Pixels.data() + Index, sizeof(Depth));
			EXPECT_FLOAT_EQ(Depth, 0.0f);
		}
		auto RedFragment = MakeShader("opaqueRed", Durin::EShaderFrequency::Fragment);
		auto IdFragment = MakeShader("idsMain", Durin::EShaderFrequency::Fragment);
		ASSERT_TRUE(RedFragment);
		ASSERT_TRUE(IdFragment);
		for (const auto Format : {Durin::EPixelFormat::R8_UNORM,
			Durin::EPixelFormat::BGRA8_UNORM,
			Durin::EPixelFormat::SBGRA8_UNORM,
			Durin::EPixelFormat::SRGBA8_UNORM,
			Durin::EPixelFormat::RGBA16_FLOAT,
			Durin::EPixelFormat::RG32_UINT})
		{
			SCOPED_TRACE(static_cast<int>(Format));
			auto FormatDesc = TextureDesc;
			FormatDesc.Format = Format;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(FormatDesc));
			auto FormatTargetResult = Durin::GDynamicRHI->RHITryCreateTexture(
				Commands, FormatDesc);
			ASSERT_TRUE(FormatTargetResult.has_value());
			auto FormatTarget = std::move(*FormatTargetResult);
			auto FormatPass = Pass;
			FormatPass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = Format;
			FormatPass.ColorRenderTargets[0] = FormatTarget.GetReference();
			auto FormatInitializer = Initializer;
			FormatInitializer.BoundShaders.FragmentShader =
				Format == Durin::EPixelFormat::RG32_UINT
					? IdFragment.GetReference() : RedFragment.GetReference();
			FormatInitializer.RenderTargetLayout = FormatPass.RenderTargetLayout;
			auto FormatPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
				"MetalColorFormat", FormatInitializer);
			ASSERT_TRUE(FormatPipeline);
			const auto FormatSignal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.BeginRenderPass(FormatPass, "MetalColorFormat");
			Commands.SetGraphicsPipelineState(*FormatPipeline);
			Commands.BindVertexBuffer(0, VertexBuffer.GetReference(), 0);
			Commands.Draw({.VertexCount = 3});
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				FormatSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			Pixels.clear();
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, FormatTarget.GetReference(), 0, 0, Pixels));
			const bool bHalf = Format == Durin::EPixelFormat::RGBA16_FLOAT;
			const bool bIds = Format == Durin::EPixelFormat::RG32_UINT;
			const bool bRedOnly = Format == Durin::EPixelFormat::R8_UNORM;
			const size_t Stride = bHalf || bIds ? 8 : bRedOnly ? 1 : 4;
			ASSERT_EQ(Pixels.size(), 8u * 8u * Stride);
			const bool bBgra = Format == Durin::EPixelFormat::BGRA8_UNORM
				|| Format == Durin::EPixelFormat::SBGRA8_UNORM;
			for (size_t Index = 0; Index < Pixels.size(); Index += Stride)
			{
				if (bRedOnly)
				{
					EXPECT_EQ(Pixels[Index], std::byte{255});
				}
				else if (bIds)
				{
					const std::array<std::byte, 8> Expected{
						std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
						std::byte{0x12}, std::byte{0xf0}, std::byte{0xde},
						std::byte{0xbc}, std::byte{0x9a}};
					for (size_t Byte = 0; Byte < Expected.size(); ++Byte)
						EXPECT_EQ(Pixels[Index + Byte], Expected[Byte]);
				}
				else if (bHalf)
				{
					EXPECT_EQ(Pixels[Index], std::byte{0});
					EXPECT_EQ(Pixels[Index + 1], std::byte{0x3c});
					EXPECT_EQ(Pixels[Index + 2], std::byte{0});
					EXPECT_EQ(Pixels[Index + 3], std::byte{0});
					EXPECT_EQ(Pixels[Index + 4], std::byte{0});
					EXPECT_EQ(Pixels[Index + 5], std::byte{0});
					EXPECT_EQ(Pixels[Index + 6], std::byte{0});
					EXPECT_EQ(Pixels[Index + 7], std::byte{0x3c});
				}
				else
				{
					EXPECT_EQ(Pixels[Index], bBgra ? std::byte{0} : std::byte{255});
					EXPECT_EQ(Pixels[Index + 1], std::byte{0});
					EXPECT_EQ(Pixels[Index + 2], bBgra ? std::byte{255} : std::byte{0});
					EXPECT_EQ(Pixels[Index + 3], std::byte{255});
				}
			}
		}
		Durin::RHIExit();
	}
}

TEST(FMetalRHIHeadlessTests, D32DepthRejectsFarDrawInBothExecutionModes)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
			"vertex float4 nearMain(uint id [[vertex_id]]) { "
			"float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; "
			"return float4(p[id],0.25,1); }\n"
			"vertex float4 farMain(uint id [[vertex_id]]) { "
			"float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; "
			"return float4(p[id],0.75,1); }\n"
			"fragment float4 greenMain() { return float4(0,1,0,1); }\n"
			"fragment float4 redMain() { return float4(1,0,0,1); }\n";
		Durin::FByteBuffer Code;
		for (char Character : Source) Code.push_back(std::byte(Character));
		auto MakeShader = [&](const char* Entry,
			Durin::EShaderFrequency Frequency) {
			auto Desc = Durin::FRHIShaderCreateDesc::Create(Entry,
				Frequency, Code, Durin::FXxHash128::HashBuffer(Code));
			Desc.Target = Durin::MetalShaderTarget;
			Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			Desc.SetEntryPoint(Entry);
			Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Frequency, Desc.MetalBindings,
				Desc.MetalPushConstantBufferSlot);
			return Durin::GDynamicRHI->RHICreateShader(Desc);
		};
		auto Near = MakeShader("nearMain", Durin::EShaderFrequency::Vertex);
		auto Far = MakeShader("farMain", Durin::EShaderFrequency::Vertex);
		auto Green = MakeShader("greenMain", Durin::EShaderFrequency::Fragment);
		auto Red = MakeShader("redMain", Durin::EShaderFrequency::Fragment);
		ASSERT_TRUE(Near);
		ASSERT_TRUE(Far);
		ASSERT_TRUE(Green);
		ASSERT_TRUE(Red);
		auto Declaration = Durin::GDynamicRHI->RHICreateVertexDeclaration({});
		ASSERT_TRUE(Declaration);
		auto ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
			"Metal depth color", 8, 8, Durin::EPixelFormat::RGBA8_UNORM);
		ColorDesc.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
			| Durin::ETextureCreateFlags::CPUReadback);
		auto DepthDesc = Durin::FRHITextureCreateDesc::Create2D(
			"Metal D32 depth", 8, 8, Durin::EPixelFormat::D32);
		DepthDesc.SetFlags(Durin::ETextureCreateFlags::DepthStencilTargetable
			| Durin::ETextureCreateFlags::CPUReadback);
		ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(DepthDesc));
		auto ColorResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, ColorDesc);
		auto DepthResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, DepthDesc);
		ASSERT_TRUE(ColorResult.has_value());
		ASSERT_TRUE(DepthResult.has_value());
		auto Color = std::move(*ColorResult);
		auto Depth = std::move(*DepthResult);
		Durin::FRHIRenderPassInfo Pass;
		Pass.RenderTargetLayout.NumColorRenderTargets = 1;
		Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
			Durin::EPixelFormat::RGBA8_UNORM;
		Pass.RenderTargetLayout.bHasDepthStencil = true;
		Pass.RenderTargetLayout.DepthStencilAttachment.Format =
			Durin::EPixelFormat::D32;
		Pass.RenderTargetLayout.DepthStencilAttachment.FinalLayout =
			Durin::ERHITextureLayout::DepthStencilAttachment;
		Pass.RenderTargetLayout.DepthStencilAttachment.FinalAccess =
			Durin::ERHIAccess::DepthStencilReadWrite;
		Pass.ColorRenderTargets[0] = Color.GetReference();
		Pass.DepthStencilRenderTarget = Depth.GetReference();
		Pass.ColorClearValues[0] = Durin::FClearValueBinding(0, 0, 0, 1);
		Pass.DepthStencilClearValue = Durin::FClearValueBinding(1.0f, 0u);
		ASSERT_TRUE(Pass.RenderTargetLayout.IsValid());
		Durin::FGraphicsPipelineStateInitializer Initializer;
		Initializer.VertexDeclaration = Declaration.GetReference();
		Initializer.RenderTargetLayout = Pass.RenderTargetLayout;
		Initializer.RasterizerState.CullMode = Durin::ERHICullMode::None;
		Initializer.DepthStencilState.bEnableTest = true;
		Initializer.DepthStencilState.bEnableWrite = true;
		Initializer.DepthStencilState.CompareOp = Durin::ERHIDepthCompareOp::Less;
		Initializer.BoundShaders = {Near.GetReference(), Green.GetReference()};
		auto NearPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalNearDepth", Initializer);
		Initializer.BoundShaders = {Far.GetReference(), Red.GetReference()};
		auto FarPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
			"MetalFarDepth", Initializer);
		ASSERT_TRUE(NearPipeline);
		ASSERT_TRUE(FarPipeline);
		const auto Signal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Commands.BeginRenderPass(Pass, "MetalD32Occlusion");
		Commands.SetGraphicsPipelineState(*NearPipeline);
		Commands.Draw({.VertexCount = 3});
		Commands.SetGraphicsPipelineState(*FarPipeline);
		Commands.Draw({.VertexCount = 3});
		Commands.EndRenderPass();
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		Durin::FByteBuffer Pixels;
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Color.GetReference(), 0, 0, Pixels));
		ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
		for (size_t Index = 0; Index < Pixels.size(); Index += 4)
		{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
		}
		Durin::FByteBuffer DepthBytes;
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Depth.GetReference(), 0, 0, DepthBytes));
		ASSERT_EQ(DepthBytes.size(), 8u * 8u * sizeof(float));
		for (size_t Offset = 0; Offset < DepthBytes.size(); Offset += sizeof(float))
		{
			float Value = 0;
			std::memcpy(&Value, DepthBytes.data() + Offset, sizeof(Value));
			EXPECT_FLOAT_EQ(Value, 0.25f);
		}
		Durin::RHIExit();
	}
}
