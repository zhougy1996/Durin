#include "../../RDGTestAccess.h"
#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "NativeTestSupport.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/Shader.h"
#include "Experimental/RHITransition.h"
#include "RDG/RDG.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanTransition.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	namespace
	{
		class FEnvironment final
		{
		public:
			FEnvironment(const char* Name, const char* Value) : Key(Name)
			{
				if (const char* Old = std::getenv(Name)) Previous = Old;
				Set(Value);
			}
			~FEnvironment() { Set(Previous ? Previous->c_str() : ""); }
		private:
			auto Set(const char* Value) -> void
			{
#ifdef _WIN32
				_putenv_s(Key.c_str(), Value);
#else
				if (*Value) setenv(Key.c_str(), Value, 1); else unsetenv(Key.c_str());
#endif
			}
			std::string Key;
			std::optional<std::string> Previous;
		};
		class FVulkanQueueQualificationTests : public ::testing::TestWithParam<const char*>
		{
		protected:
			auto SetUp() -> void override
			{
				Backend = std::make_unique<FEnvironment>("DURIN_RHI_BACKEND", "vulkan");
				Execution = std::make_unique<FEnvironment>("DURIN_RHI_EXECUTION", GetParam());
				Split = std::make_unique<FEnvironment>("DURIN_VULKAN_SPLIT_BARRIERS", "1");
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
				ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
				FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::Graphics);
			}
			auto TearDown() -> void override
			{
				if (GDynamicRHI) RHIExit();
				Split.reset(); Execution.reset(); Backend.reset();
			}
		private:
			std::unique_ptr<FEnvironment> Backend, Execution, Split;
		};
		struct FColorParameters final
		{
			FRDGColorAttachmentBinding Color;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				static const std::array Members{MakeRDGColorAttachmentBindingMetadata<FColorParameters, decltype(Color)>(
					"Color", offsetof(FColorParameters, Color))};
				static const auto Metadata = MakeInlineRDGParametersMetadata<FColorParameters>("FQueueColorParameters", Members);
				return &Metadata;
			}
		};
	}

	TEST(FVulkanQueueAdmissionTests, MissingDrawParametersKeepsDirectComputeButDisablesIndirectGraphics)
	{
		FVulkanPhysicalDeviceCandidateInput Input;
		Input.ApiVersion = VK_API_VERSION_1_3;
		Input.bFillModeNonSolid = Input.bIndependentBlend = true;
		Input.MaxImageDimension2D = Input.MaxImageDimensionCube = 4096;
		Input.MaxImageArrayLayers = 256;
		Input.MaxComputeWorkGroupCount = {65535, 65535, 65535};
		Input.bRequirePresentation = false;
		Input.AvailableExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		Input.QueueFamilies = {{vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 1, false}};
		auto Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_FALSE(Result.bEnableShaderDrawParameters);
		Input.bShaderDrawParameters = true;
		EXPECT_TRUE(EvaluateVulkanPhysicalDeviceCandidate(Input).bEnableShaderDrawParameters);
		Input.bRequirePortabilitySubset = true;
		EXPECT_FALSE(EvaluateVulkanPhysicalDeviceCandidate(Input).IsSuitable());
	}

	TEST_P(FVulkanQueueQualificationTests, NativeSplitMatchesFullComputeBufferAndImage)
	{
		ASSERT_TRUE(GDynamicRHI->RHIGetQueueCapabilities().bSplitBarriers)
			<< "Native event qualification requires a supporting ICD; fallback is a different gate.";
		FShaderCompileOptions Options;
		Options.Target = VulkanShaderTarget;
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		const auto Code = IShaderBuildModule::Get()->CompileMounted("/Engine/Qualification/QueueProducer", Options);
		ASSERT_TRUE(Code) << Code.Diagnostic;
		auto Shader = GDynamicRHI->RHICreateShader(MakeShaderCreateDesc(Code.CompiledShaders[0]));
		ASSERT_TRUE(Shader);
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		ASSERT_TRUE(BuildPipelineLayoutFromShaders(Code.CompiledShaders, Initializer.PipelineLayout));
		auto Pipeline = GDynamicRHI->RHICreateComputePipelineState("QueueQualification", Initializer);
		ASSERT_TRUE(Pipeline);
		auto& Commands = FRHICommandListImmediate::Get();
		std::array<FByteBuffer, 2> Reference;
		for (uint32 Shape = 0; Shape < 3; ++Shape)
		for (bool Split : {false, true})
		{
			SCOPED_TRACE(::testing::Message() << "shape=" << Shape << " split=" << Split << " mode=" << GetParam());
			auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create("EventBuffer", 512, 4,
				EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy));
			ASSERT_TRUE(Buffer);
			auto View = GDynamicRHI->RHICreateBufferView(Buffer,
				{.Offset = 256, .Size = 256, .Type = ERHIBufferViewType::StructuredStorage});
			ASSERT_TRUE(View);
			auto Image = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("EventImage", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::Storage | ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback));
			auto Readback = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("EventBufferReadback", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback));
			ASSERT_TRUE(Image); ASSERT_TRUE(Readback);
			const FRHITextureSubresourceRange Whole{ERHITextureAspect::Color, 0, 1, 0, 1};
			Commands.BeginGPUSubmission({.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.TransitionBuffers(std::array{FRHIBufferTransition{Buffer, 256, 256, ERHIAccess::None, ERHIAccess::ComputeShaderReadWrite}});
			Commands.TransitionTextures(std::array{FRHITextureTransition{Image, Whole, ERHIAccess::Discard, ERHIAccess::ComputeShaderReadWrite},
				FRHITextureTransition{Readback, Whole, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			Commands.SwitchPipeline(ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			const std::array Parameters{FRHIShaderParameterResource{.Resource = View.GetReference(), .BindingIndex = 0, .Type = ERHIBindingType::StorageBuffer},
				FRHIShaderParameterResource{.Resource = Image.GetReference(), .BindingIndex = 1, .Type = ERHIBindingType::StorageImage}};
			Commands.SetShaderParameters(Shader, Parameters);
			Commands.Dispatch(1, 1, 1);
			Commands.SwitchPipeline(ERHIPipeline::Graphics);
			FRHITransitionDesc Desc;
			const FRHIBufferTransition BufferBarrier{Buffer, 256, 256, ERHIAccess::ComputeShaderReadWrite, ERHIAccess::TransferRead};
			const FRHITextureTransition ImageBarrier{Image, Whole, ERHIAccess::ComputeShaderReadWrite, ERHIAccess::TransferRead};
			if (Shape != 1) Desc.Buffers.push_back(BufferBarrier);
			if (Shape != 0) Desc.Textures.push_back(ImageBarrier);
			if (Shape == 1) Commands.TransitionBuffers(std::array{BufferBarrier});
			if (Shape == 0) Commands.TransitionTextures(std::array{ImageBarrier});
			std::shared_ptr<FRHITransition> Transition;
			if (Split)
			{
				Transition = GDynamicRHI->RHICreateTransition(Desc);
				ASSERT_TRUE(Transition);
				Commands.BeginTransition(Transition);
				Commands.EndTransition(Transition);
			}
			else
			{
				if (!Desc.Buffers.empty()) Commands.TransitionBuffers(Desc.Buffers);
				if (!Desc.Textures.empty()) Commands.TransitionTextures(Desc.Textures);
			}
			Commands.CopyBufferToTexture(Buffer, Readback, std::array{FRHIBufferTextureCopyRegion{
				.BufferOffset = 256, .TextureExtent = {8, 8, 1}}});
			Commands.EndGPUSubmission();
			std::weak_ptr<FRHITransition> Weak = Transition;
			Transition.reset();
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			if (Split)
			{
				ASSERT_FALSE(Weak.expired()) << "Recorded native event must survive RHI replay until GPU retirement.";
				auto Native = std::static_pointer_cast<FVulkanTransition>(Weak.lock());
				ASSERT_TRUE(Native->GetEvent()) << "This gate must actually create a Vulkan event.";
			}
			std::array<FByteBuffer, 2> Actual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Readback, 0, 0, Actual[0]));
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Image, 0, 0, Actual[1]));
			ASSERT_EQ(Actual[0].size(), 256u); ASSERT_EQ(Actual[1].size(), 256u);
			for (uint32 Index = 0; Index < 256; ++Index)
			{
				EXPECT_EQ(Actual[0][Index], std::byte(Index / 4 + 1));
				EXPECT_EQ(Actual[1][Index], std::byte(Index % 4 == 1 || Index % 4 == 3 ? 255 : 0));
			}
			if (Split) EXPECT_EQ(Actual, Reference); else Reference = Actual;
		}
		std::cout << "NATIVE_SPLIT_EQUIVALENCE mode=" << GetParam() << " pairs=3 buffers=3 images=3\n";
	}

	TEST_P(FVulkanQueueQualificationTests, RDGSplitPlanExecutesAcrossNativeSubmissionBatches)
	{
		ASSERT_TRUE(GDynamicRHI->RHIGetQueueCapabilities().bSplitBarriers);
		auto& Commands = FRHICommandListImmediate::Get();
		auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create("GraphEventBuffer", 512, 4,
			EBufferUsageFlags::DestinationCopy | EBufferUsageFlags::SourceCopy));
		auto Output = GDynamicRHI->RHICreateTexture(Commands,
			FRHITextureCreateDesc::Create2D("GraphEventOutput", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback));
		ASSERT_TRUE(Buffer); ASSERT_TRUE(Output);
		Commands.TransitionBuffers(std::array{FRHIBufferTransition{Buffer, 0, 512, ERHIAccess::None, ERHIAccess::TransferRead}});
		const FRHITextureSubresourceRange Whole{ERHITextureAspect::Color, 0, 1, 0, 1};
		Commands.TransitionTextures(std::array{FRHITextureTransition{Output, Whole, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
		FRDGBuilder Graph;
		const auto B = Graph.RegisterExternalBuffer(Buffer, "Buffer", ERHIAccess::TransferRead, ERHIAccess::TransferRead);
		const auto T = Graph.RegisterExternalTexture(Output, "Output", ERHIAccess::TransferWrite, ERHIAccess::TransferRead);
		const FByteBuffer Expected(256, std::byte{123});
		const auto Producer = FRDGBuilderTestAccessor::AddPass(Graph, "Producer", ERDGPassType::Copy,
			[&](FRHICommandListImmediate& C, const FRDGPassResources&) { C.WriteBuffer(Buffer, 256, Expected); });
		FRDGBuilderTestAccessor::UseBuffer(Graph, Producer, B, 256, 256, ERDGUse::Write, ERHIAccess::TransferWrite);
		Graph.MarkPassRoot(Producer, "native split producer");
		for (uint32 I = 0; I < 7; ++I)
		{
			const auto Independent = FRDGBuilderTestAccessor::AddPass(Graph, std::format("Independent{}", I), ERDGPassType::Graphics);
			Graph.MarkPassRoot(Independent, "bounded native batch boundary");
		}
		const auto Consumer = FRDGBuilderTestAccessor::AddPass(Graph, "Consumer", ERDGPassType::Copy,
			[&](FRHICommandListImmediate& C, const FRDGPassResources&) { C.CopyBufferToTexture(Buffer, Output,
				std::array{FRHIBufferTextureCopyRegion{.BufferOffset = 256, .TextureExtent = {8, 8, 1}}}); });
		FRDGBuilderTestAccessor::UseBuffer(Graph, Consumer, B, 256, 256, ERDGUse::Read, ERHIAccess::TransferRead);
		FRDGBuilderTestAccessor::UseTexture(Graph, Consumer, T, Whole, ERDGUse::Write, ERHIAccess::TransferWrite);
		Graph.MarkPassRoot(Consumer, "native readback");
		const auto Result = Graph.Execute(Commands);
		ASSERT_TRUE(Result) << ToString(Result.error());
		ASSERT_EQ(Graph.GetExecutionPlan().SplitBarriers.size(), 2u);
		EXPECT_GE(Graph.GetExecutionPlan().Batches.size(), 2u);
		FByteBuffer Actual;
		ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Output, 0, 0, Actual));
		EXPECT_EQ(Actual, Expected);
		std::cout << "RDG_NATIVE_SPLIT mode=" << GetParam() << " split_batches=2 bytes=" << Actual.size() << '\n';
	}

	TEST_P(FVulkanQueueQualificationTests, NativeAliasingPreservesBuffersAndImagesAcrossGraphs)
	{
		ASSERT_TRUE(GDynamicRHI->RHISupportsTransientAliasing());
		auto& Commands = FRHICommandListImmediate::Get();
		auto Desc = FRHIBufferCreateDesc::Create("AliasBuffer", 256, 4,
			EBufferUsageFlags::DestinationCopy | EBufferUsageFlags::SourceCopy);
		Desc.Aliasing = ERHITransientAliasing::Enabled;
		auto Root = GDynamicRHI->RHICreateBuffer(Commands, Desc);
		auto Alias = Root ? GDynamicRHI->RHICreateAliasedBuffer(Desc, Root) : FBufferRHIRef{};
		ASSERT_TRUE(Root); ASSERT_TRUE(Alias);
		EXPECT_NE(FVulkanBuffer::Cast(Root)->GetHandle(), FVulkanBuffer::Cast(Alias)->GetHandle());
		EXPECT_TRUE(FVulkanBuffer::Cast(Root)->SharesBackingAllocationWith(*FVulkanBuffer::Cast(Alias)));
		auto Readback = GDynamicRHI->RHICreateTexture(Commands,
			FRHITextureCreateDesc::Create2D("AliasBufferReadback", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback));
		ASSERT_TRUE(Readback);
		for (uint32 Reuse = 0; Reuse < 4; ++Reuse)
		{
			const FByteBuffer Initial(256, std::byte{17}), Expected(256, std::byte(91 + Reuse));
			Commands.AliasingBarrier({nullptr, Root});
			Commands.WriteBuffer(Root, 0, Initial);
			Commands.AliasingBarrier({Root, Alias});
			Commands.WriteBuffer(Alias, 0, Expected);
			Commands.TransitionBuffers(std::array{FRHIBufferTransition{Alias, 0, 256, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}});
			Commands.TransitionTextures(std::array{FRHITextureTransition{Readback, {ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			Commands.CopyBufferToTexture(Alias, Readback, std::array{FRHIBufferTextureCopyRegion{.TextureExtent = {8, 8, 1}}});
			FByteBuffer Actual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Readback, 0, 0, Actual));
			EXPECT_EQ(Actual, Expected);
		}
		class FAllocator final : public FRDGAllocator
		{
		public:
			FTextureRHIRef Root;
			std::map<uint32, FTextureRHIRef> Textures;
			auto Allocate(std::span<const FRDGAllocationRequest> Requests, FRDGAllocatedResources& Output) -> FRDGAllocationResult override
			{
				Textures.clear();
				for (const auto& R : Requests)
				{
					auto Desc = FRHITextureCreateDesc::Create("AliasImage", R.TextureDesc.Dimension);
					static_cast<FRHITextureDesc&>(Desc) = R.TextureDesc;
					Desc.Aliasing = ERHITransientAliasing::Enabled;
					if (R.AliasSourceId == UINT32_MAX)
					{
						if (!Root) Root = GDynamicRHI->RHICreateTexture(FRHICommandListImmediate::Get(), Desc);
						Textures[R.ResourceId] = Root;
					}
					else Textures[R.ResourceId] = GDynamicRHI->RHICreateAliasedTexture(Desc, Textures.at(R.AliasSourceId));
					if (!Output.SetTexture(R.ResourceId, Textures[R.ResourceId], 1, "fixture", R.AliasSourceId))
						return std::unexpected(FRDGAllocationFailure{ERDGAllocationError::PhysicalAllocationFailed, R.ResourceId});
				}
				return {};
			}
		} Allocator;
		for (uint32 Reuse = 0; Reuse < 4; ++Reuse)
		{
			FRDGBuilder Graph;
			Graph.SetTransientAliasingEnabled(true);
			for (uint32 Index = 0; Index < 3; ++Index)
			{
				const auto Color = Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D("AliasImage", 8, 8, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::CPUReadback)}, std::format("Color{}", Index));
				auto P = Graph.AllocParameters<FColorParameters>();
				P->Color = {Color, {ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIRenderTargetLoadAction::Clear};
				FRDGRasterPassOptions Options;
				Options.ColorClearValues[0] = FClearValueBinding(Reuse % 2 ? 0.0f : 1.0f, Reuse % 2 ? 1.0f : 0.0f, Index / 2.0f, 1.0f);
				Graph.AddRasterPass(std::format("Clear{}", Index), std::move(P),
					[](FRHICommandListImmediate&, const FColorParameters&, const FRDGParameterResolver&) {}, Options);
			}
			const auto Result = Graph.Execute(Commands, &Allocator);
			ASSERT_TRUE(Result) << ToString(Result.error());
			EXPECT_EQ(Graph.GetExecutionPlan().AliasingHandoffs.size(), 2u);
			for (const auto& [Id, Texture] : Allocator.Textures)
				EXPECT_TRUE(static_cast<FVulkanTexture*>(Texture.GetReference())->SharesBackingAllocationWith(*static_cast<FVulkanTexture*>(Allocator.Root.GetReference())));
			FByteBuffer Pixels;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Allocator.Textures.rbegin()->second, 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 256u);
			for (size_t I = 0; I < Pixels.size(); I += 4)
			{
				EXPECT_EQ(Pixels[I], Reuse % 2 ? std::byte{0} : std::byte{255});
				EXPECT_EQ(Pixels[I + 1], Reuse % 2 ? std::byte{255} : std::byte{0});
				EXPECT_EQ(Pixels[I + 2], std::byte{255}); EXPECT_EQ(Pixels[I + 3], std::byte{255});
			}
		}
		std::cout << "NATIVE_ALIASING mode=" << GetParam() << " buffer_reuses=4 graph_reuses=4 image_handoffs=8\n";
	}
	INSTANTIATE_TEST_SUITE_P(ReplayModes, FVulkanQueueQualificationTests,
		::testing::Values("inline", "threaded"), [](const auto& Info) { return std::string(Info.param); });
}
