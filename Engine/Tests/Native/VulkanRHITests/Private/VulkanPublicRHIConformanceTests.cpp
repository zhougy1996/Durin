#include "VulkanIntegrationTestFixture.h"
#include "VulkanPipelinePrecacheTestSupport.h"
#include "VulkanTestSupport.h"
#include "VulkanRHITestEnvironment.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SlangShaderCompiler.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanViewport.h"
#include "VulkanRHIPrivate.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	class FVulkanPublicRHIConformanceTests : public FVulkanIntegrationTest
	{
	};

	TEST_F(FVulkanPublicRHIConformanceTests, DynamicOffsetsReuseSparseArrayDescriptorsAndSelectCorrectPixels)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "RecoverableResourceFactories.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentDynamicMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		for (const bool FullValidation : {false, true})
		for (const uint32 Variant : {0u, 1u, 2u})
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			SCOPED_TRACE(FullValidation);
			SCOPED_TRACE(Variant);
			const bool Deferred = Variant != 0;
			_putenv_s("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION", FullValidation ? "on" : "off");
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			auto& Commands = FRHICommandListImmediate::Get();
			{
				auto CreateShader = [&](uint32 Index) {
					const auto& Shader = Compiled.CompiledShaders[Index];
					auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code, Shader.Hash);
					Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
					return GDynamicRHI->RHICreateShader(Desc);
				};
				auto Vertex = CreateShader(0);
				auto Fragment = CreateShader(1);
				auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
				ASSERT_TRUE(Vertex && Fragment && Declaration);
				FRHIRenderTargetLayout Layout;
				Layout.NumColorRenderTargets = 1;
				auto& Attachment = Layout.ColorAttachments[0].RenderTarget;
				Attachment.Format = EPixelFormat::RGBA8_UNORM;
				Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
				Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
				Attachment.InitialLayout = ERHITextureLayout::Undefined;
				Attachment.InitialAccess = ERHIAccess::None;
				Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
				Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = Layout;
				Initializer.BoundShaders.VertexShader = Vertex;
				Initializer.BoundShaders.FragmentShader = Fragment;
				Initializer.VertexDeclaration = Declaration;
				Initializer.PipelineLayout.BindingLayouts.resize(3);
				Initializer.PipelineLayout.BindingLayouts[2].BindingLayouts.emplace_back(
					EShaderStageFlags::Fragment, 5, ERHIBindingType::UniformBufferDynamic, 2);
				auto Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState("SparseDynamicOffsets", Initializer);
				ASSERT_TRUE(Pipeline);
				auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("DynamicOffsetPixels", 8, 8, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
				ASSERT_TRUE(Target);
				const std::array Colors{std::array<float, 4>{1, 0, 0, 1}, std::array<float, 4>{0, 1, 0, 1}, std::array<float, 4>{0, 0, 1, 1}};
				const uint32 Alignment = static_cast<uint32>(GetVulkanDeviceForTest(*static_cast<FVulkanDynamicRHI*>(GDynamicRHI))->GetGpuProperties().limits.minUniformBufferOffsetAlignment);
				const uint32 Stride = std::max(Alignment, 16u);
				auto Buffer = Deferred ? TRefCountPtr<FRHIBuffer>{} : GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create(
					"DynamicOffsetColors", Stride * 3, 16, EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Static));
				auto* Bytes = Deferred ? nullptr : static_cast<std::byte*>(GDynamicRHI->RHILockBuffer(Commands, Buffer, 0, Stride * 3, EResourceLockMode::WriteOnly));
				ASSERT_TRUE(Deferred || Bytes);
				std::array<FRHIUniformBufferRange, 3> Uniforms;
				for (uint32 Index = 0; Index < Colors.size(); ++Index)
				{
					if (Variant == 2)
					{
						FByteBuffer Data(Stride * 3);
						std::memcpy(Data.data() + Stride * 2, Colors[Index].data(), sizeof(Colors[Index]));
						auto Logical = Commands.CreateUniformBuffer({Stride * 3}, ERHIBufferLifetimeUsage::SingleFrame, Data);
						ASSERT_TRUE(Logical);
						Uniforms[Index] = {Logical.GetReference(), Stride * 2, 16, Logical.GetReference()};
					}
					else if (Deferred) Uniforms[Index] = Commands.CreateUniformBufferRange(Colors[Index].data(), sizeof(Colors[Index]));
					else
					{
						std::memcpy(Bytes + Stride * Index, Colors[Index].data(), sizeof(Colors[Index]));
						Uniforms[Index] = {Buffer.GetReference(), Stride * Index, sizeof(Colors[Index])};
					}
				}
				if (!Deferred)
				{
					GDynamicRHI->RHIUnlockBuffer(Commands, Buffer);
					ASSERT_EQ(Uniforms[0].Buffer, Uniforms[2].Buffer);
					ASSERT_NE(Uniforms[0].Offset, Uniforms[2].Offset);
				}
				std::array<FRHIShaderParameterResource, 2> Parameters;
				for (uint32 Index = 0; Index < Parameters.size(); ++Index)
					Parameters[1 - Index] = {.Resource = Uniforms[Index].Buffer, .SetIndex = 2, .BindingIndex = 5,
						.ArrayElement = Index, .Type = ERHIBindingType::UniformBufferDynamic,
						.Offset = Uniforms[Index].Offset, .Size = Uniforms[Index].Size};
				auto First = FRHIShaderParameterBatch::Create(Fragment, Parameters);
				Parameters[1].Resource = Uniforms[2].Buffer;
				Parameters[1].Offset = Uniforms[2].Offset;
				auto Second = FRHIShaderParameterBatch::Create(Fragment, Parameters);
				ASSERT_TRUE(First && Second);
				if (!Deferred) ASSERT_EQ(First->GetParameters()[1].Resource, Second->GetParameters()[1].Resource);
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout = Layout;
				Pass.ColorRenderTargets[0] = Target;
				Commands.BeginRenderPass(Pass, "DynamicOffsetArrays");
				Commands.SetGraphicsPipelineState(*Pipeline);
				Commands.SetViewport(0, 0, 0, 4, 8, 1);
				Commands.SetPreparedShaderParameters(First);
				Commands.Draw({.VertexCount = 3});
				Commands.SetViewport(4, 0, 0, 8, 8, 1);
				Commands.SetPreparedShaderParameters(Second);
				Commands.Draw({.VertexCount = 3});
				Commands.SetPreparedShaderParameters(Second);
				Commands.Draw({.VertexCount = 3});
				Commands.EndRenderPass();
				// Submitted commands must retain the captured allocations after producer owners release them.
				First.reset();
				Second.reset();
				Uniforms = {};
				FByteBuffer Pixels;
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels));
				ASSERT_EQ(Pixels.size(), 256u);
				const size_t Left = (4 * 8 + 1) * 4;
				const size_t Right = (4 * 8 + 6) * 4;
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Left]), 128, 1);
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Left + 1]), 128, 1);
				EXPECT_EQ(Pixels[Left + 2], std::byte{0});
				EXPECT_EQ(Pixels[Right], std::byte{0});
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Right + 1]), 128, 1);
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Right + 2]), 128, 1);
				FRHIDiagnosticSnapshot Snapshot;
				GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
					Snapshot = GDynamicRHI->RHIGetDiagnosticSnapshot();
				});
				// The two sparse empty sets share one descriptor; both color bindings share another.
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.NativeCreations, 2u);
			}
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanPublicRHIConformanceTests, DescriptorSetsReuseCommonStateAcrossMaterialAndCompatiblePipelineChanges)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "RecoverableResourceFactories.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentFrequencyMain", "FragmentFrequencyAlternateMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			auto& Commands = FRHICommandListImmediate::Get();
			{
				auto CreateShader = [&](uint32 Index) {
					const auto& Shader = Compiled.CompiledShaders[Index];
					auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code, Shader.Hash);
					Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
					return GDynamicRHI->RHICreateShader(Desc);
				};
				auto Vertex = CreateShader(0);
				auto Fragment = CreateShader(1);
				auto AlternateFragment = CreateShader(2);
				auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
				ASSERT_TRUE(Vertex && Fragment && AlternateFragment && Declaration);
				FRHIRenderTargetLayout Layout;
				Layout.NumColorRenderTargets = 1;
				auto& Attachment = Layout.ColorAttachments[0].RenderTarget;
				Attachment.Format = EPixelFormat::RGBA8_UNORM;
				Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
				Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
				Attachment.InitialLayout = ERHITextureLayout::Undefined;
				Attachment.InitialAccess = ERHIAccess::None;
				Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
				Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = Layout;
				Initializer.BoundShaders.VertexShader = Vertex;
				Initializer.BoundShaders.FragmentShader = Fragment;
				Initializer.VertexDeclaration = Declaration;
				Initializer.PipelineLayout.BindingLayouts.resize(3);
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
					EShaderStageFlags::Fragment, 3, ERHIBindingType::UniformBuffer);
				Initializer.PipelineLayout.BindingLayouts[2].BindingLayouts.emplace_back(
					EShaderStageFlags::Fragment, 7, ERHIBindingType::UniformBuffer);
				auto Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState("FrequencySets", Initializer);
				Initializer.BoundShaders.FragmentShader = AlternateFragment;
				auto Compatible = GDynamicRHI->RHICreateGraphicsPipelineState("CompatibleFrequencySets", Initializer);
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.clear();
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
					EShaderStageFlags::Vertex | EShaderStageFlags::Fragment, 3, ERHIBindingType::UniformBuffer);
				auto Incompatible = GDynamicRHI->RHICreateGraphicsPipelineState("IncompatibleFrequencySet", Initializer);
				ASSERT_TRUE(Pipeline && Compatible && Incompatible);
				ASSERT_NE(Pipeline.GetReference(), Compatible.GetReference());
				ASSERT_NE(Compatible.GetReference(), Incompatible.GetReference());
				auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("DynamicOffsetPixels", 8, 8, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
				ASSERT_TRUE(Target);
				const std::array Colors{std::array<float, 4>{1, 1, 1, 1}, std::array<float, 4>{1, 0, 0, 1}, std::array<float, 4>{0, 1, 0, 1}};
				std::array<FBufferRHIRef, 3> Buffers;
				for (uint32 Index = 0; Index < Buffers.size(); ++Index)
				{
					Buffers[Index] = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create(
						"FrequencyColor", 16, 16, EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Static));
					ASSERT_TRUE(Buffers[Index]);
					auto* Bytes = GDynamicRHI->RHILockBuffer(Commands, Buffers[Index], 0, 16, EResourceLockMode::WriteOnly);
					ASSERT_NE(Bytes, nullptr);
					std::memcpy(Bytes, Colors[Index].data(), 16);
					GDynamicRHI->RHIUnlockBuffer(Commands, Buffers[Index]);
				}
				auto MakeBatch = [&](FRHIShader* Shader, uint32 BufferIndex, uint32 Set, uint32 Binding) {
					const FRHIShaderParameterResource Resource{.Resource = Buffers[BufferIndex].GetReference(),
						.SetIndex = Set, .BindingIndex = Binding, .Type = ERHIBindingType::UniformBuffer, .Size = 16};
					return FRHIShaderParameterBatch::Create(Shader, std::span<const FRHIShaderParameterResource>(&Resource, 1));
				};
				auto Common = MakeBatch(Fragment, 0, 0, 3);
				auto Red = MakeBatch(Fragment, 1, 2, 7);
				auto Green = MakeBatch(Fragment, 2, 2, 7);
				auto AlternateCommon = MakeBatch(AlternateFragment, 0, 0, 3);
				auto AlternateRed = MakeBatch(AlternateFragment, 1, 2, 7);
				ASSERT_TRUE(Common && Red && Green && AlternateCommon && AlternateRed);
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout = Layout;
				Pass.ColorRenderTargets[0] = Target;
				Commands.BeginRenderPass(Pass, "DynamicOffsetArrays");
				Commands.SetGraphicsPipelineState(*Pipeline);
				Commands.SetViewport(0, 0, 0, 2, 8, 1);
				Commands.SetPreparedShaderParameters(Common);
				Commands.SetPreparedShaderParameters(Red);
				Commands.Draw({.VertexCount = 3});
				Commands.SetViewport(2, 0, 0, 4, 8, 1);
				Commands.SetPreparedShaderParameters(Green);
				Commands.Draw({.VertexCount = 3});
				Commands.SetGraphicsPipelineState(*Compatible);
				Commands.SetGraphicsPipelineState(*Compatible);
				Commands.SetViewport(4, 0, 0, 6, 8, 1);
				Commands.SetPreparedShaderParameters(AlternateCommon);
				Commands.SetPreparedShaderParameters(AlternateRed);
				Commands.Draw({.VertexCount = 3});
				Commands.SetGraphicsPipelineState(*Incompatible);
				Commands.SetViewport(6, 0, 0, 8, 8, 1);
				Commands.SetPreparedShaderParameters(AlternateCommon);
				Commands.SetPreparedShaderParameters(AlternateRed);
				Commands.Draw({.VertexCount = 3});
				Commands.EndRenderPass();
				FByteBuffer Pixels;
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels));
				ASSERT_EQ(Pixels.size(), 256u);
				for (uint32 Column = 0; Column < 4; ++Column)
				{
					const size_t Pixel = (4 * 8 + Column * 2) * 4;
					EXPECT_EQ(Pixels[Pixel], Column == 1 ? std::byte{0} : std::byte{255});
					EXPECT_EQ(Pixels[Pixel + 1], Column == 1 ? std::byte{255} : std::byte{0});
					EXPECT_EQ(Pixels[Pixel + 2], std::byte{0});
				}
				FRHIDiagnosticSnapshot Snapshot;
				GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
					Snapshot = GDynamicRHI->RHIGetDiagnosticSnapshot();
				});
				// Common, empty, red, green, and the incompatible common layout.
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.NativeCreations, 5u);
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.Occupancy, 5u);
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorValueOccupancy, 4u);
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.Hits, 7u);

				// Freeze three complete command chunks before any pool retirement.
				// Each must initialize all sparse sets and remain replayable after the
				// preceding chunk's descriptor allocation lease has retired.
				std::array<FRHICommandList, 3> Chunks;
				for (uint32 Index = 0; Index < Chunks.size(); ++Index)
				{
					auto& Chunk = Chunks[Index];
					Chunk.SwitchPipeline(ERHIPipeline::Graphics);
					Chunk.BeginRenderPass(Pass, "FrozenDescriptorChunk");
					Chunk.SetGraphicsPipelineState(*Pipeline);
					Chunk.SetViewport(0, 0, 0, 8, 8, 1);
					Chunk.SetPreparedShaderParameters(Common);
					Chunk.SetPreparedShaderParameters(Index == 1 ? Green : Red);
					Chunk.Draw({.VertexCount = 3});
					Chunk.EndRenderPass();
					Chunk.FinishRecording();
				}
				for (uint32 Index = 0; Index < Chunks.size(); ++Index)
				{
					if (Index != 0)
						GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
							SubmitAndRetireDescriptorPoolsForTesting();
							WaitForAllVulkanSubmissionsForTesting();
						});
					Commands.QueueCommandList(std::move(Chunks[Index]));
					ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels));
					ASSERT_EQ(Pixels.size(), 256u);
					const size_t Center = (4 * 8 + 4) * 4;
					EXPECT_EQ(Pixels[Center], Index == 1 ? std::byte{0} : std::byte{255});
					EXPECT_EQ(Pixels[Center + 1], Index == 1 ? std::byte{255} : std::byte{0});
					GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
						Snapshot = GDynamicRHI->RHIGetDiagnosticSnapshot();
					});
					EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.NativeCreations, 5u + Index * 3u);
				}

			}
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanPublicRHIConformanceTests,
		PublicRHIConformanceDrawMatchesPixelsAndDiagnosticsAcrossModes)
	{
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
		ASSERT_TRUE(CompileOutput) << FormatShaderError(CompileOutput.Error);
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 2u);

		std::array<Durin::FByteBuffer, 2> ModePixels;
		std::array<FRHIDiagnosticSnapshot, 2> ModeSnapshots;
		const std::array Modes{"inline", "threaded"};
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		for (size_t ModeIndex = 0; ModeIndex < Modes.size(); ++ModeIndex)
		{
			SCOPED_TRACE(Modes[ModeIndex]);
			_putenv_s("DURIN_RHI_EXECUTION", Modes[ModeIndex]);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();

			auto MakeShaderDesc = [](const FCompiledShader& Shader) {
				FRHIShaderCreateDesc Desc = FRHIShaderCreateDesc::Create(
					Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code,
					Shader.Hash);
				Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
				return Desc;
			};
			FShaderRHIRef VertexShader = GDynamicRHI->RHICreateShader(
				MakeShaderDesc(CompileOutput.CompiledShaders[0]));
			FShaderRHIRef FragmentShader = GDynamicRHI->RHICreateShader(
				MakeShaderDesc(CompileOutput.CompiledShaders[1]));
			FVertexDeclarationRHIRef VertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration({});
			ASSERT_TRUE(VertexShader && FragmentShader && VertexDeclaration);

			FRHITextureCreateDesc SampledDesc = FRHITextureCreateDesc::Create2D(
				"ConformanceSampledTexture", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Sampled =
				GDynamicRHI->RHICreateTexture(Commands, SampledDesc);
			ASSERT_TRUE(Sampled);
			const std::array<uint8, 64> SampledBytes = [] {
				std::array<uint8, 64> Bytes{};
				Bytes.fill(0x7f);
				return Bytes;
			}();
			Commands.UpdateTexture2D(Sampled, 0, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, 4, 4), 16,
				std::as_bytes(std::span{SampledBytes}));
			Commands.TransitionTextures(std::array{Durin::FRHITextureTransition{
				Sampled, {Durin::ERHITextureAspect::Color, 0, 1, 0, 1},
				Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}});
			FTextureViewRHIRef SampledView = GDynamicRHI->RHICreateTextureView(
				Sampled, MakeDefaultTextureViewDesc(
					*Sampled, ERHITextureViewUsage::Sampled));
			TRefCountPtr<FRHISampler> Sampler =
				GDynamicRHI->RHICreateSampler(FRHISamplerDesc{});
			ASSERT_TRUE(SampledView && Sampler);

			FRHIRenderTargetLayout Layout;
			Layout.NumColorRenderTargets = 1;
			auto& Attachment = Layout.ColorAttachments[0].RenderTarget;
			Attachment.Format = EPixelFormat::RGBA8_UNORM;
			Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
			Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
			Attachment.InitialLayout = ERHITextureLayout::Undefined;
			Attachment.InitialAccess = ERHIAccess::None;
			Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = Layout;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = VertexDeclaration;
			auto& Bindings = Initializer.PipelineLayout.BindingLayouts
				.emplace_back().BindingLayouts;
			Bindings.emplace_back(
				EShaderStageFlags::Vertex, 0, ERHIBindingType::Texture);
			Bindings.emplace_back(
				EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler, 2);
			FGraphicsPipelineStateRHIRef Pipeline =
				GDynamicRHI->RHICreateGraphicsPipelineState(
					"PublicRHIConformancePipeline", Initializer);
			ASSERT_TRUE(Pipeline);

			const FRHICapabilities* Capabilities = GDynamicRHI->RHIGetCapabilities();
			ASSERT_NE(Capabilities, nullptr);
			FRHITextureCreateDesc TargetDesc = FRHITextureCreateDesc::Create2D(
				"PublicRHIConformanceTarget", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::CPUReadback);
			FTextureRHIRef Target =
				GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
			ASSERT_TRUE(Target);
			FGPUTimingQueryRHIRef Timing =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Timing);

			FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout = Layout;
			Pass.ColorRenderTargets[0] = Target;
			Pass.ColorClearValues[0] = FClearValueBinding(0, 0, 0, 1);
			Commands.BeginDiagnosticRegion("PublicRHI.Conformance.Draw");
			Commands.BeginGPUTimingQuery(Timing);
			Commands.SwitchPipeline(ERHIPipeline::Graphics);
			Commands.BeginRenderPass(Pass, "PublicRHIConformancePass");
			Commands.SetGraphicsPipelineState(*Pipeline);
			Commands.SetViewport(0, 0, 0, 8, 8, 1);
			std::array<FRHIShaderParameterResource, 1> TextureParameters{
				FRHIShaderParameterResource{.Resource = SampledView.GetReference(),
					.SetIndex = 0, .BindingIndex = 0, .ArrayElement = 0,
					.Type = ERHIBindingType::Texture}};
			std::array<FRHIShaderParameterResource, 2> SamplerParameters{
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 0,
					.Type = ERHIBindingType::Sampler},
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 1,
					.Type = ERHIBindingType::Sampler}};
			Commands.SetShaderParameters(VertexShader, TextureParameters);
			Commands.SetShaderParameters(FragmentShader, SamplerParameters);
			Commands.Draw({.VertexCount = 3});
			Commands.SetShaderParameters(VertexShader, TextureParameters);
			Commands.SetShaderParameters(FragmentShader, SamplerParameters);
			Commands.Draw({.VertexCount = 3});
			for (uint32 SnapshotIndex = 1; SnapshotIndex <= 512; ++SnapshotIndex)
			{
				SamplerParameters[0].Offset = SnapshotIndex;
				Commands.SetShaderParameters(FragmentShader, SamplerParameters);
				Commands.Draw({.VertexCount = 3});
			}
			Commands.EndRenderPass();
			Commands.EndGPUTimingQuery(Timing);
			Commands.EndDiagnosticRegion();
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				Commands, Target, 0, 0, ModePixels[ModeIndex]));
			ASSERT_EQ(ModePixels[ModeIndex].size(), 8u * 8u * 4u);
			EXPECT_NEAR(std::to_integer<uint8>(ModePixels[ModeIndex][0]), 64, 1);
			EXPECT_NEAR(std::to_integer<uint8>(ModePixels[ModeIndex][1]), 128, 1);
			EXPECT_NEAR(std::to_integer<uint8>(ModePixels[ModeIndex][2]), 191, 1);
			EXPECT_EQ(ModePixels[ModeIndex][3], std::byte{255});

			Target = nullptr;
			TargetDesc.DebugName = "PublicRHIConformanceReplacement";
			FTextureRHIRef Replacement =
				GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
			ASSERT_TRUE(Replacement);
			Commands.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				PollVulkanGPUTimingForTest(
					*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
				ModeSnapshots[ModeIndex] =
					GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Timing).State,
				ERHIGPUTimingResultState::Ready);
			EXPECT_GT(ModeSnapshots[ModeIndex].Naming.LabelBegins, 0u);
			EXPECT_GT(ModeSnapshots[ModeIndex].Timing.ReadyResultCount, 0u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].Naming.ActiveRegionDepth, 0u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorSnapshots.Occupancy, 512u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorValueOccupancy, 1536u);
			EXPECT_GE(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorSnapshots.Hits, 1u);
			EXPECT_GE(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorSnapshots.Evictions, 1u);

			FRHIDiagnosticSnapshot StatisticsReset;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				GDynamicRHI->RHIResetDiagnosticStatistics();
				StatisticsReset = GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(StatisticsReset.PipelineCache.DescriptorSnapshots.Occupancy, 512u);
			EXPECT_EQ(StatisticsReset.PipelineCache.DescriptorValueOccupancy, 1536u);
			EXPECT_EQ(StatisticsReset.PipelineCache.DescriptorSnapshots.Hits, 0u);

			Replacement = nullptr;
			Timing = nullptr;
			Pipeline = nullptr;
			Sampler = nullptr;
			SampledView = nullptr;
			Sampled = nullptr;
			FragmentShader = nullptr;
			VertexShader = nullptr;
			VertexDeclaration = nullptr;
			Commands.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}

		EXPECT_EQ(ModePixels[0], ModePixels[1]);
		EXPECT_EQ(ModeSnapshots[0].Timing.IntervalCapacity,
			ModeSnapshots[1].Timing.IntervalCapacity);
		EXPECT_EQ(ModeSnapshots[0].Naming.ActiveRegionDepth,
			ModeSnapshots[1].Naming.ActiveRegionDepth);
	}
}
