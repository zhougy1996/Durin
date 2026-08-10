#include <gtest/gtest.h>

#include "PCH.VulkanRHI.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Shader/SlangShaderCompiler.h"
#include "Texture/TextureBuild.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDiagnostics.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"
#include "VulkanView.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TestMipCount = 3;
		constexpr uint32 TestTextureCount = 6;
		constexpr uint32 ResultCount = TestMipCount * TestTextureCount;

		struct FFloat4
		{
			float X;
			float Y;
			float Z;
			float W;
		};

		struct FVulkanBufferAllocation
		{
			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			void* MappedData = nullptr;
		};

		auto FindMemoryType(
			VkPhysicalDevice PhysicalDevice,
			uint32 TypeBits,
			VkMemoryPropertyFlags RequiredProperties
		) -> uint32
		{
			VkPhysicalDeviceMemoryProperties Properties{};
			vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &Properties);
			for (uint32 Index = 0; Index < Properties.memoryTypeCount; ++Index)
			{
				if ((TypeBits & (1u << Index)) != 0
					&& (Properties.memoryTypes[Index].propertyFlags & RequiredProperties) == RequiredProperties)
				{
					return Index;
				}
			}
			return UINT32_MAX;
		}

		auto CreateReadbackBuffer(
			VkDevice Device,
			VkPhysicalDevice PhysicalDevice,
			VkDeviceSize Size
		) -> FVulkanBufferAllocation
		{
			FVulkanBufferAllocation Allocation;
			const VkBufferCreateInfo BufferInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = Size,
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE
			};
			if (vkCreateBuffer(Device, &BufferInfo, nullptr, &Allocation.Buffer) != VK_SUCCESS)
			{
				return {};
			}

			VkMemoryRequirements Requirements{};
			vkGetBufferMemoryRequirements(Device, Allocation.Buffer, &Requirements);
			const uint32 MemoryType = FindMemoryType(
				PhysicalDevice,
				Requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
			);
			if (MemoryType == UINT32_MAX)
			{
				vkDestroyBuffer(Device, Allocation.Buffer, nullptr);
				return {};
			}

			const VkMemoryAllocateInfo AllocateInfo{
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = Requirements.size,
				.memoryTypeIndex = MemoryType
			};
			if (vkAllocateMemory(Device, &AllocateInfo, nullptr, &Allocation.Memory) != VK_SUCCESS
				|| vkBindBufferMemory(Device, Allocation.Buffer, Allocation.Memory, 0) != VK_SUCCESS
				|| vkMapMemory(Device, Allocation.Memory, 0, Size, 0, &Allocation.MappedData) != VK_SUCCESS)
			{
				if (Allocation.Memory != VK_NULL_HANDLE)
				{
					vkFreeMemory(Device, Allocation.Memory, nullptr);
				}
				vkDestroyBuffer(Device, Allocation.Buffer, nullptr);
				return {};
			}
			std::memset(Allocation.MappedData, 0, static_cast<size_t>(Size));
			return Allocation;
		}

		auto SrgbToLinear(float Value) -> float
		{
			return Value <= 0.04045f
				? Value / 12.92f
				: std::pow((Value + 0.055f) / 1.055f, 2.4f);
		}

		auto MakeSolidMip(uint32 Size, const std::array<uint8, 4>& Color) -> std::vector<uint8>
		{
			std::vector<uint8> Pixels(static_cast<size_t>(Size) * Size * 4);
			for (size_t Offset = 0; Offset < Pixels.size(); Offset += 4)
			{
				std::copy(Color.begin(), Color.end(), Pixels.begin() + static_cast<std::ptrdiff_t>(Offset));
			}
			return Pixels;
		}

		auto CreateAndUploadTexture(
			FRHICommandListImmediate& RHICmdList,
			EPixelFormat Format,
			const std::array<std::array<uint8, 4>, TestMipCount>& Colors
		) -> FTextureRHIRef
		{
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("VulkanSamplingTest")
				.SetExtent(FIntPoint(4, 4))
				.SetNumMips(TestMipCount)
				.SetFormat(Format)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::DestinationCopy);
			FTextureRHIRef Texture = RHICreateTexture(Desc);
			if (!Texture)
			{
				return nullptr;
			}

			for (uint32 MipIndex = 0; MipIndex < TestMipCount; ++MipIndex)
			{
				const uint32 Size = std::max(1u, 4u >> MipIndex);
				const std::vector<uint8> Pixels = MakeSolidMip(Size, Colors[MipIndex]);
				GDynamicRHI->RHIUpdateTexture2D(
					RHICmdList,
					Texture,
					MipIndex,
					0,
					FUpdateTextureRegion2D(0, 0, 0, 0, Size, Size),
					Size * 4,
					Pixels.data()
				);
			}
			return Texture;
		}

		auto BuildSolidCompressedMipChain(
			ETextureUsage Usage,
			bool bSrgb,
			bool bHasTransparency,
			const std::array<std::array<uint8, 4>, TestMipCount>& Colors
		) -> FTexturePlatformData
		{
			FTexturePlatformData Combined;
			for (uint32 MipIndex = 0; MipIndex < TestMipCount; ++MipIndex)
			{
				const uint32 Size = std::max(1u, 4u >> MipIndex);
				FTextureSourceData Source;
				Source.Width = Size;
				Source.Height = Size;
				Source.SourceChannelCount = 4;
				Source.Format = ETextureSourceFormat::RGBA8;
				Source.bHasTransparency = bHasTransparency;
				Source.Pixels = MakeSolidMip(Size, Colors[MipIndex]);

				FTexturePlatformData Built;
				std::string Error;
				if (!TextureBuild::BuildMipChain(Source, Usage, bSrgb, Built, Error))
				{
					ADD_FAILURE() << Error;
					return {};
				}
				if (MipIndex == 0)
				{
					Combined.PixelFormat = Built.PixelFormat;
				}
				if (Built.PixelFormat != Combined.PixelFormat || Built.Mips.empty())
				{
					ADD_FAILURE() << "Compressed mip build returned an inconsistent format or no mip data.";
					return {};
				}
				Combined.Mips.push_back(Built.Mips.front());
			}
			return Combined;
		}

		auto CreateAndUploadPlatformTexture(
			FRHICommandListImmediate& RHICmdList,
			const FTexturePlatformData& PlatformData
		) -> FTextureRHIRef
		{
			if (!PlatformData.IsValid())
			{
				return nullptr;
			}
			const FTexture2DMipData& BaseMip = PlatformData.Mips.front();
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("VulkanCompressedSamplingTest")
				.SetExtent(FIntPoint(BaseMip.Width, BaseMip.Height))
				.SetNumMips(static_cast<uint32>(PlatformData.Mips.size()))
				.SetFormat(PlatformData.PixelFormat)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::DestinationCopy);
			FTextureRHIRef Texture = RHICreateTexture(Desc);
			for (uint32 MipIndex = 0; Texture && MipIndex < PlatformData.Mips.size(); ++MipIndex)
			{
				const FTexture2DMipData& Mip = PlatformData.Mips[MipIndex];
				GDynamicRHI->RHIUpdateTexture2D(
					RHICmdList,
					Texture,
					MipIndex,
					0,
					FUpdateTextureRegion2D(0, 0, 0, 0, Mip.Width, Mip.Height),
					Mip.RowPitch,
					Mip.Pixels.data()
				);
			}
			return Texture;
		}

		auto ExpectColorNear(
			const FFloat4& Actual,
			const std::array<uint8, 4>& Encoded,
			bool bSrgb,
			float Tolerance = 0.002f,
			bool bForceOpaqueAlpha = false
		) -> void
		{
			const auto Decode = [bSrgb](uint8 Value, bool bAlpha) {
				const float Normalized = static_cast<float>(Value) / 255.0f;
				return bSrgb && !bAlpha ? SrgbToLinear(Normalized) : Normalized;
			};
			EXPECT_NEAR(Actual.X, Decode(Encoded[0], false), Tolerance);
			EXPECT_NEAR(Actual.Y, Decode(Encoded[1], false), Tolerance);
			EXPECT_NEAR(Actual.Z, Decode(Encoded[2], false), Tolerance);
			EXPECT_NEAR(Actual.W, bForceOpaqueAlpha ? 1.0f : Decode(Encoded[3], true), Tolerance);
		}
	}

	TEST(FVulkanTextureSamplingTests,
		InlineNativeComputeUploadsAndSamplesEveryMipWithKnownColorSpaceValues)
	{
#if !DURIN_WITH_EDITOR
		GTEST_SKIP() << "Offline texture compression is an editor-only capability.";
#endif
		struct FInlineRHIScope
		{
			FInlineRHIScope()
			{
				_putenv_s("DURIN_RHI_EXECUTION", "inline");
			}

			~FInlineRHIScope()
			{
				if (GDynamicRHI)
				{
					RHIExit();
				}
				_putenv_s("DURIN_RHI_EXECUTION", "");
			}
		} Scope;

		ASSERT_TRUE(RHIInit());
		ASSERT_NE(GDynamicRHI, nullptr);
		auto* VulkanRHI = static_cast<VulkanRHI::IVulkanDynamicRHI*>(GDynamicRHI);
		const VkDevice Device = VulkanRHI->RHIGetVkDevice();
		const VkPhysicalDevice PhysicalDevice = VulkanRHI->RHIGetVkPhysicalDevice();
		ASSERT_NE(Device, VK_NULL_HANDLE);
		ASSERT_NE(PhysicalDevice, VK_NULL_HANDLE);

		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		GDynamicRHI->RHIBeginFrame_RenderThread(RHICmdList);
		RHICmdList.SwitchPipeline(ERHIPipeline::Graphics);

		const std::array<std::array<uint8, 4>, TestMipCount> Colors{{
			{128, 64, 32, 255},
			{64, 128, 192, 224},
			{255, 128, 0, 160}
		}};
		FTextureRHIRef LinearTexture = CreateAndUploadTexture(RHICmdList, EPixelFormat::RGBA8_UNORM, Colors);
		FTextureRHIRef SrgbTexture = CreateAndUploadTexture(RHICmdList, EPixelFormat::SRGBA8_UNORM, Colors);
		FTextureRHIRef Bc1Texture = CreateAndUploadPlatformTexture(
			RHICmdList, BuildSolidCompressedMipChain(ETextureUsage::Color, true, false, Colors));
		FTextureRHIRef Bc3Texture = CreateAndUploadPlatformTexture(
			RHICmdList, BuildSolidCompressedMipChain(ETextureUsage::Color, true, true, Colors));
		FTextureRHIRef Bc5Texture = CreateAndUploadPlatformTexture(
			RHICmdList, BuildSolidCompressedMipChain(ETextureUsage::Normal, false, false, Colors));
		FTextureRHIRef Bc7Texture = CreateAndUploadPlatformTexture(
			RHICmdList, BuildSolidCompressedMipChain(ETextureUsage::DataMask, false, false, Colors));
		ASSERT_TRUE(LinearTexture);
		ASSERT_TRUE(SrgbTexture);
		ASSERT_TRUE(Bc1Texture);
		ASSERT_TRUE(Bc3Texture);
		ASSERT_TRUE(Bc5Texture);
		ASSERT_TRUE(Bc7Texture);

		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_TEST_DATA_DIR) / "VulkanTextureSampling.slang";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {"computeMain"};
		CompileOptions.Frequencies = {EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput CompileOutput = Compiler.Compile(ShaderPath.string(), CompileOptions);
		ASSERT_TRUE(CompileOutput) << CompileOutput.ErrorMessage;
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 1u);
		const FCompiledShader& CompiledShader = CompileOutput.CompiledShaders[0];

		const VkDeviceSize ResultBytes = sizeof(FFloat4) * ResultCount;
		FVulkanBufferAllocation Readback = CreateReadbackBuffer(Device, PhysicalDevice, ResultBytes);
		ASSERT_NE(Readback.Buffer, VK_NULL_HANDLE);
		ASSERT_NE(Readback.MappedData, nullptr);

		const std::array<VkDescriptorSetLayoutBinding, 8> LayoutBindings{{
			{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
		}};
		const VkDescriptorSetLayoutCreateInfo LayoutInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = static_cast<uint32>(LayoutBindings.size()),
			.pBindings = LayoutBindings.data()
		};
		VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
		ASSERT_EQ(vkCreateDescriptorSetLayout(Device, &LayoutInfo, nullptr, &DescriptorSetLayout), VK_SUCCESS);

		const VkPipelineLayoutCreateInfo PipelineLayoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &DescriptorSetLayout
		};
		VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
		ASSERT_EQ(vkCreatePipelineLayout(Device, &PipelineLayoutInfo, nullptr, &PipelineLayout), VK_SUCCESS);

		const VkShaderModuleCreateInfo ShaderInfo{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = CompiledShader.Code->size(),
			.pCode = reinterpret_cast<const uint32*>(CompiledShader.Code->data())
		};
		VkShaderModule ShaderModule = VK_NULL_HANDLE;
		ASSERT_EQ(vkCreateShaderModule(Device, &ShaderInfo, nullptr, &ShaderModule), VK_SUCCESS);
		const VkPipelineShaderStageCreateInfo StageInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = ShaderModule,
			.pName = CompiledShader.BinaryEntryPoint.c_str()
		};
		const VkComputePipelineCreateInfo PipelineInfo{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = StageInfo,
			.layout = PipelineLayout
		};
		VkPipeline Pipeline = VK_NULL_HANDLE;
		ASSERT_EQ(vkCreateComputePipelines(Device, VK_NULL_HANDLE, 1, &PipelineInfo, nullptr, &Pipeline), VK_SUCCESS);

		const std::array<VkDescriptorPoolSize, 3> PoolSizes{{
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TestTextureCount},
			{VK_DESCRIPTOR_TYPE_SAMPLER, 1},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}
		}};
		const VkDescriptorPoolCreateInfo PoolInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 1,
			.poolSizeCount = static_cast<uint32>(PoolSizes.size()),
			.pPoolSizes = PoolSizes.data()
		};
		VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
		ASSERT_EQ(vkCreateDescriptorPool(Device, &PoolInfo, nullptr, &DescriptorPool), VK_SUCCESS);
		const VkDescriptorSetAllocateInfo SetAllocateInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = DescriptorPool,
			.descriptorSetCount = 1,
			.pSetLayouts = &DescriptorSetLayout
		};
		VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
		ASSERT_EQ(vkAllocateDescriptorSets(Device, &SetAllocateInfo, &DescriptorSet), VK_SUCCESS);

		const VkSamplerCreateInfo SamplerInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.maxLod = static_cast<float>(TestMipCount - 1)
		};
		VkSampler Sampler = VK_NULL_HANDLE;
		ASSERT_EQ(vkCreateSampler(Device, &SamplerInfo, nullptr, &Sampler), VK_SUCCESS);

		const std::array<FRHITexture*, TestTextureCount> Textures{
			LinearTexture.GetReference(), SrgbTexture.GetReference(), Bc1Texture.GetReference(),
			Bc3Texture.GetReference(), Bc5Texture.GetReference(), Bc7Texture.GetReference()};
		std::array<FTextureViewRHIRef, TestTextureCount> TextureViews;
		std::array<VkDescriptorImageInfo, TestTextureCount> ImageInfos{};
		for (uint32 TextureIndex = 0; TextureIndex < TestTextureCount; ++TextureIndex)
		{
			TextureViews[TextureIndex] = GDynamicRHI->RHICreateTextureView(
				Textures[TextureIndex], MakeDefaultTextureViewDesc(
					*Textures[TextureIndex], ERHITextureViewUsage::Sampled));
			ASSERT_TRUE(TextureViews[TextureIndex]);
			ImageInfos[TextureIndex] = {VK_NULL_HANDLE,
				static_cast<VulkanRHI::FVulkanTextureView*>(TextureViews[TextureIndex].GetReference())->GetHandle(),
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		}
		const VkDescriptorImageInfo SamplerDescriptorInfo{.sampler = Sampler};
		const VkDescriptorBufferInfo BufferInfo{Readback.Buffer, 0, ResultBytes};
		std::array<VkWriteDescriptorSet, 8> Writes{};
		for (uint32 TextureIndex = 0; TextureIndex < TestTextureCount; ++TextureIndex)
		{
			Writes[TextureIndex] = {
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, DescriptorSet, TextureIndex, 0, 1,
				VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ImageInfos[TextureIndex]
			};
		}
		Writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, DescriptorSet, 6, 0, 1,
			VK_DESCRIPTOR_TYPE_SAMPLER, &SamplerDescriptorInfo};
		Writes[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, DescriptorSet, 7, 0, 1,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &BufferInfo};
		vkUpdateDescriptorSets(Device, static_cast<uint32>(Writes.size()), Writes.data(), 0, nullptr);

		// Compute is not yet part of the portable command surface. This explicit
		// Vulkan integration path is independent of the RHI recorder. Replay the
		// portable uploads first so native compute follows them in the same context.
		VulkanRHI->RHIExecuteCommandBufferForBackendIntegration(
			[Pipeline, PipelineLayout, DescriptorSet](vk::CommandBuffer CommandBuffer) {
				const VkCommandBuffer RawCommandBuffer = CommandBuffer;
				vkCmdBindPipeline(RawCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, Pipeline);
				vkCmdBindDescriptorSets(RawCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout, 0, 1, &DescriptorSet, 0, nullptr);
				vkCmdDispatch(RawCommandBuffer, 1, 1, 1);
				const VkMemoryBarrier HostBarrier{
					.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
					.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
					.dstAccessMask = VK_ACCESS_HOST_READ_BIT
				};
				vkCmdPipelineBarrier(
					RawCommandBuffer,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_PIPELINE_STAGE_HOST_BIT,
					0,
					1,
					&HostBarrier,
					0,
					nullptr,
					0,
					nullptr);
			});

		GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
		GDynamicRHI->RHIBlockUntilGPUIdle();

		const auto Results = std::span(static_cast<const FFloat4*>(Readback.MappedData), ResultCount);
		for (uint32 MipIndex = 0; MipIndex < TestMipCount; ++MipIndex)
		{
			ExpectColorNear(Results[MipIndex], Colors[MipIndex], false);
			ExpectColorNear(Results[TestMipCount + MipIndex], Colors[MipIndex], true);
			ExpectColorNear(Results[TestMipCount * 2 + MipIndex], Colors[MipIndex], true, 0.01f, true);
			ExpectColorNear(Results[TestMipCount * 3 + MipIndex], Colors[MipIndex], true, 0.01f);
			EXPECT_NEAR(Results[TestMipCount * 4 + MipIndex].X, static_cast<float>(Colors[MipIndex][0]) / 255.0f, 0.03f);
			EXPECT_NEAR(Results[TestMipCount * 4 + MipIndex].Y, static_cast<float>(Colors[MipIndex][1]) / 255.0f, 0.03f);
			ExpectColorNear(Results[TestMipCount * 5 + MipIndex], Colors[MipIndex], false, 0.01f);
		}

		vkDestroySampler(Device, Sampler, nullptr);
		vkDestroyDescriptorPool(Device, DescriptorPool, nullptr);
		vkDestroyPipeline(Device, Pipeline, nullptr);
		vkDestroyShaderModule(Device, ShaderModule, nullptr);
		vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(Device, DescriptorSetLayout, nullptr);
		vkUnmapMemory(Device, Readback.Memory);
		vkDestroyBuffer(Device, Readback.Buffer, nullptr);
		vkFreeMemory(Device, Readback.Memory, nullptr);
		TextureViews = {};
		LinearTexture = nullptr;
		SrgbTexture = nullptr;
		Bc1Texture = nullptr;
		Bc3Texture = nullptr;
		Bc5Texture = nullptr;
		Bc7Texture = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		RHICmdList.SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FVulkanTextureSamplingTests, ThreadedResourceCreationAndUniformOverflowStayRHIThreadOwned)
	{
		struct FThreadedRHIScope
		{
			FThreadedRHIScope()
			{
				_putenv_s("DURIN_RHI_EXECUTION", "threaded");
			}

			~FThreadedRHIScope()
			{
				if (GDynamicRHI)
				{
					RHIExit();
				}
				_putenv_s("DURIN_RHI_EXECUTION", "");
			}
		} Scope;

		ASSERT_TRUE(RHIInit());
		VulkanRHI::ResetVulkanMemoryBaselineStatistics();
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
		const uint64 InitialFrameNumber =
			GCommandListExecutor.GetFrameNumber();
		auto BeginFrame = []() {
			ENQUEUE_RENDER_COMMAND(BeginThreadedVulkanTestFrame)(
				[](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
		};
		auto EndFrame = []() {
			ENQUEUE_RENDER_COMMAND(EndThreadedVulkanTestFrame)(
				[](FRHICommandListImmediate& CommandList) {
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					CommandList.ImmediateFlush(
						EImmediateFlushType::FlushRHIThread);
				});
			FlushRenderingCommands();
		};
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		BeginFrame();

		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"ThreadedCreationBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		EXPECT_TRUE(Buffer);
		auto* LockedBuffer = static_cast<uint8*>(GDynamicRHI->RHILockBuffer(
			RHICmdList, Buffer.GetReference(), 32, 16,
			EResourceLockMode::WriteOnly));
		ASSERT_NE(LockedBuffer, nullptr);
		std::fill_n(LockedBuffer, 16, 0x3c);
		GDynamicRHI->RHIUnlockBuffer(RHICmdList, Buffer.GetReference());

		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"ThreadedCreationTexture", 4, 4, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Flags = ETextureCreateFlags::ShaderResource
			| ETextureCreateFlags::CPUReadback;
		FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc);
		EXPECT_TRUE(Texture);
		std::array<uint8, 4 * 4 * 4> TextureBytes{};
		for (uint32 Index = 0; Index < TextureBytes.size(); ++Index)
		{
			TextureBytes[Index] = static_cast<uint8>(Index);
		}
		const FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, 4, 4);
		GDynamicRHI->RHIUpdateTexture2D(
			RHICmdList, Texture.GetReference(), 0, 0, UpdateRegion,
			4 * 4, TextureBytes.data());
		std::vector<uint8> ReadbackBytes;
		EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(
			RHICmdList, Texture.GetReference(), 0, 0, ReadbackBytes));
		EXPECT_EQ(ReadbackBytes, std::vector<uint8>(
			TextureBytes.begin(), TextureBytes.end()));

		FRHISamplerDesc SamplerDesc;
		TRefCountPtr<FRHISampler> Sampler = GDynamicRHI->RHICreateSampler(SamplerDesc);
		EXPECT_TRUE(Sampler);

		const uint64 SynchronousOperationsBeforeUniforms =
			GCommandListExecutor.GetStats().SynchronousOperationCount;
		const std::array<uint8, 256> SmallUniformData{};
		const FRHIUniformBufferRange SmallUniformRange =
			RHICmdList.AllocateDynamicUniformBuffer(
				SmallUniformData.data(),
				static_cast<uint32>(SmallUniformData.size()));
		EXPECT_NE(SmallUniformRange.Buffer, nullptr);
		EXPECT_EQ(SmallUniformRange.Offset, 0u);
		EXPECT_EQ(GCommandListExecutor.GetStats().SynchronousOperationCount,
			SynchronousOperationsBeforeUniforms);

		std::vector<uint8> OversizedUniformData(4 * 1024 * 1024 + 256, 0x5a);
		const FRHIUniformBufferRange UniformRange =
			RHICmdList.AllocateDynamicUniformBuffer(
				OversizedUniformData.data(),
				static_cast<uint32>(OversizedUniformData.size()));
		EXPECT_NE(UniformRange.Buffer, nullptr);
		EXPECT_EQ(UniformRange.Offset, 0u);
		EXPECT_EQ(UniformRange.Size, OversizedUniformData.size());
		EXPECT_EQ(GCommandListExecutor.GetStats().SynchronousOperationCount,
			SynchronousOperationsBeforeUniforms + 1);

		GDynamicRHI->RHIBlockUntilGPUIdle();
		EndFrame();
		EXPECT_EQ(
			GCommandListExecutor.GetFrameNumber(), InitialFrameNumber + 1);
		VulkanRHI::FVulkanBackendPoolTestStats FirstFramePoolStats;
		VulkanRHI::FVulkanCompletionTestStats FirstFrameCompletionStats;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			FirstFramePoolStats = VulkanRHI::GetVulkanBackendPoolTestStats();
			FirstFrameCompletionStats = VulkanRHI::GetVulkanCompletionTestStats();
		});
		EXPECT_NE(std::ranges::find(
			FirstFramePoolStats.DynamicUniformTokens,
			FirstFrameCompletionStats.LastSubmittedToken),
			FirstFramePoolStats.DynamicUniformTokens.end());

		BeginFrame();
		const FRHIUniformBufferRange SecondSlotUniformRange =
			RHICmdList.AllocateDynamicUniformBuffer(
				SmallUniformData.data(),
				static_cast<uint32>(SmallUniformData.size()));
		EXPECT_NE(SecondSlotUniformRange.Buffer, SmallUniformRange.Buffer);
		EXPECT_EQ(SecondSlotUniformRange.Offset, 0u);
		EndFrame();
		EXPECT_EQ(
			GCommandListExecutor.GetFrameNumber(), InitialFrameNumber + 2);

		BeginFrame();
		const FRHIUniformBufferRange ReusedFirstSlotUniformRange =
			RHICmdList.AllocateDynamicUniformBuffer(
				SmallUniformData.data(),
				static_cast<uint32>(SmallUniformData.size()));
		EXPECT_EQ(ReusedFirstSlotUniformRange.Buffer, SmallUniformRange.Buffer);
		EXPECT_EQ(ReusedFirstSlotUniformRange.Offset, 0u);
		EndFrame();
		EXPECT_EQ(
			GCommandListExecutor.GetFrameNumber(), InitialFrameNumber + 3);

		constexpr uint32 ChurnFrameCount = 16;
		bool bChurnSucceeded = true;
		for (uint32 FrameIndex = 0; FrameIndex < ChurnFrameCount; ++FrameIndex)
		{
			ENQUEUE_RENDER_COMMAND(ThreadedVulkanResourceChurn)(
				[FrameIndex, &bChurnSucceeded](FRHICommandListImmediate& CommandList) {
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);

					const FRHIBufferCreateDesc ChurnBufferDesc =
						FRHIBufferCreateDesc::Create(
							"ThreadedChurnBuffer", 256, 16,
							EBufferUsageFlags::VertexBuffer
								| EBufferUsageFlags::Static);
					FBufferRHIRef ChurnBuffer = GDynamicRHI->RHICreateBuffer(
						CommandList, ChurnBufferDesc);

					FRHITextureCreateDesc ChurnTextureDesc =
						FRHITextureCreateDesc::Create2D(
							"ThreadedChurnTexture", 4, 4,
							EPixelFormat::RGBA8_UNORM);
					ChurnTextureDesc.Flags = ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::CPUReadback;
					FTextureRHIRef ChurnTexture = GDynamicRHI->RHICreateTexture(
						CommandList, ChurnTextureDesc);
					std::array<uint8, 4 * 4 * 4> ChurnBytes{};
					for (uint32 ByteIndex = 0; ByteIndex < ChurnBytes.size(); ++ByteIndex)
					{
						ChurnBytes[ByteIndex] = static_cast<uint8>(
							FrameIndex + ByteIndex);
					}
					const FUpdateTextureRegion2D ChurnRegion(0, 0, 0, 0, 4, 4);
					if (ChurnBuffer && ChurnTexture)
					{
						GDynamicRHI->RHIUpdateTexture2D(
							CommandList, ChurnTexture.GetReference(), 0, 0,
							ChurnRegion, 4 * 4, ChurnBytes.data());
						std::vector<uint8> ChurnReadback;
						bChurnSucceeded = bChurnSucceeded
							&& GDynamicRHI->RHIReadTexture2D(
								CommandList, ChurnTexture.GetReference(), 0, 0,
								ChurnReadback)
							&& ChurnReadback == std::vector<uint8>(
								ChurnBytes.begin(), ChurnBytes.end());
					}
					else
					{
						bChurnSucceeded = false;
					}

					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					ChurnBuffer = nullptr;
					ChurnTexture = nullptr;
					CommandList.ImmediateFlush(
						EImmediateFlushType::FlushRHIThreadFlushResources);
				});
		}
		FlushRenderingCommands();
		EXPECT_TRUE(bChurnSucceeded);
		EXPECT_EQ(GCommandListExecutor.GetFrameNumber(),
			InitialFrameNumber + 3 + ChurnFrameCount);
		Buffer = nullptr;
		Texture = nullptr;
		Sampler = nullptr;
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);

		const VulkanRHI::FVulkanMemoryBaselineStatistics Baseline =
			VulkanRHI::GetVulkanMemoryBaselineStatistics();
		const auto ClassStats = [&Baseline](
			VulkanRHI::EVulkanAllocationClassCandidate Candidate)
			-> const VulkanRHI::FVulkanAllocationCandidateStatistics& {
			return Baseline.AllocationClasses[static_cast<uint32>(Candidate)];
		};
		EXPECT_GE(Baseline.UploadOperationCount, 18u);
		EXPECT_GE(Baseline.UploadBytes, 16u + 17u * 64u);
		EXPECT_GE(Baseline.ReadbackOperationCount, 17u);
		EXPECT_GE(Baseline.ReadbackBytes, 17u * 64u);
		EXPECT_GT(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::DeviceLocal)
			.AllocationCount, 0u);
		EXPECT_EQ(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::TransferUpload)
			.AllocationCount, 1u);
		EXPECT_EQ(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::TransferReadback)
			.AllocationCount, 1u);
		EXPECT_EQ(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::TransferUpload)
			.ArenaCapacityBytes, 8ull * 1024 * 1024);
		EXPECT_EQ(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::TransferReadback)
			.ArenaCapacityBytes, 4ull * 1024 * 1024);
		EXPECT_GE(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::TransferUpload)
			.ArenaReuseCount, 17u);
		EXPECT_GE(ClassStats(
			VulkanRHI::EVulkanAllocationClassCandidate::TransferReadback)
			.ArenaReuseCount, 16u);
		EXPECT_GT(Baseline.DeferredDeleteHighWater, 0u);
		EXPECT_GT(Baseline.HeapCount, 0u);
		for (uint32 HeapIndex = 0; HeapIndex < Baseline.HeapCount; ++HeapIndex)
		{
			EXPECT_GT(Baseline.HeapBudgetBytes[HeapIndex], 0u);
		}
	}

	TEST(FVulkanTextureSamplingTests, CreatesExactCountedBufferAndTextureViews)
	{
		struct FInlineRHIScope
		{
			FInlineRHIScope() { _putenv_s("DURIN_RHI_EXECUTION", "inline"); }
			~FInlineRHIScope()
			{
				if (GDynamicRHI) RHIExit();
				_putenv_s("DURIN_RHI_EXECUTION", "");
			}
		} Scope;

		ASSERT_TRUE(RHIInit());
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(RHICmdList,
			FRHIBufferCreateDesc::Create("FormattedView", 64, 0,
				EBufferUsageFlags::FormattedBuffer));
		ASSERT_TRUE(Buffer);
		const FRHIBufferViewDesc BufferViewDesc{
			4, 16, ERHIBufferViewType::Formatted, EPixelFormat::R32_FLOAT};
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		VulkanRHI::ArmVulkanCreateFailure(VulkanRHI::EVulkanCreateFailurePoint::BufferView);
		EXPECT_FALSE(GDynamicRHI->RHICreateBufferView(Buffer, BufferViewDesc));
#endif
		FBufferViewRHIRef BufferView = GDynamicRHI->RHICreateBufferView(
			Buffer, BufferViewDesc);
		ASSERT_TRUE(BufferView);
		EXPECT_TRUE(static_cast<VulkanRHI::FVulkanBufferView*>(
			BufferView.GetReference())->GetHandle());

		FTextureRHIRef Cube = GDynamicRHI->RHICreateTexture(RHICmdList,
			FRHITextureCreateDesc::CreateCube("FaceView")
				.SetExtent(16)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		ASSERT_TRUE(Cube);
		FRHITextureViewDesc FaceDesc = MakeDefaultTextureViewDesc(
			*Cube, ERHITextureViewUsage::Sampled);
		FaceDesc.Dimension = ERHITextureViewDimension::Texture2D;
		FaceDesc.Range.FirstArrayLayer = 3;
		FaceDesc.Range.NumArrayLayers = 1;
		FTextureViewRHIRef FaceView = GDynamicRHI->RHICreateTextureView(
			Cube, FaceDesc);
		ASSERT_TRUE(FaceView);
		EXPECT_EQ(FaceView->GetDesc(), FaceDesc);
		EXPECT_TRUE(static_cast<VulkanRHI::FVulkanTextureView*>(
			FaceView.GetReference())->GetHandle());
	}

	TEST(FVulkanTextureSamplingTests, PublicCopyMatrixPreservesExactBytesInlineAndThreaded)
	{
		for (const char* Mode : {"inline", "threaded"})
		{
			struct FRHIScope
			{
				explicit FRHIScope(const char* InMode) { _putenv_s("DURIN_RHI_EXECUTION", InMode); }
				~FRHIScope()
				{
					if (GDynamicRHI) RHIExit();
					_putenv_s("DURIN_RHI_EXECUTION", "");
				}
			} Scope(Mode);
			ASSERT_TRUE(RHIInit()) << Mode;
			FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();

			const FRHITextureCreateDesc SourceDesc = FRHITextureCreateDesc::Create2D(
				"CopySource", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
			const FRHITextureCreateDesc MiddleDesc = FRHITextureCreateDesc::Create2D(
				"CopyMiddle", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy);
			const FRHITextureCreateDesc FinalDesc = FRHITextureCreateDesc::Create2D(
				"CopyFinal", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback |
					ETextureCreateFlags::ShaderResource);
			FTextureRHIRef SourceTexture = GDynamicRHI->RHICreateTexture(RHICmdList, SourceDesc);
			FTextureRHIRef MiddleTexture = GDynamicRHI->RHICreateTexture(RHICmdList, MiddleDesc);
			FTextureRHIRef FinalTexture = GDynamicRHI->RHICreateTexture(RHICmdList, FinalDesc);
			FBufferRHIRef FirstBuffer = GDynamicRHI->RHICreateBuffer(RHICmdList,
				FRHIBufferCreateDesc::Create("FirstCopyBuffer", 64, 4,
					EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
			FBufferRHIRef SecondBuffer = GDynamicRHI->RHICreateBuffer(RHICmdList,
				FRHIBufferCreateDesc::Create("SecondCopyBuffer", 64, 4,
					EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
			ASSERT_TRUE(SourceTexture && MiddleTexture && FinalTexture && FirstBuffer && SecondBuffer);

			std::array<uint8, 64> Expected{};
			for (uint32 Index = 0; Index < Expected.size(); ++Index)
				Expected[Index] = static_cast<uint8>(Index * 3 + 1);
			GDynamicRHI->RHIUpdateTexture2D(RHICmdList, SourceTexture, 0, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, 4, 4), 16, Expected.data());

			const FRHITextureSubresourceRange WholeColor{ERHITextureAspect::Color, 0, 1, 0, 1};
			RHICmdList.TransitionTextures(std::array{
				FRHITextureTransition{SourceTexture, WholeColor,
					ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
				FRHITextureTransition{MiddleTexture, WholeColor,
					ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			const std::array TextureRegions{
				FRHITextureCopyRegion{.Extent = {2, 4, 1}},
				FRHITextureCopyRegion{
					.SourceOffset = {2, 0, 0},
					.DestinationOffset = {2, 0, 0},
					.Extent = {2, 4, 1}}};
			RHICmdList.CopyTexture(SourceTexture, MiddleTexture, TextureRegions);
			RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
				MiddleTexture, WholeColor, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}});
			RHICmdList.TransitionBuffers(std::array{FRHIBufferTransition{
				FirstBuffer, 0, 64, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			const std::array BufferTextureRegion{FRHIBufferTextureCopyRegion{
				.TextureExtent = {4, 4, 1}}};
			RHICmdList.CopyTextureToBuffer(MiddleTexture, FirstBuffer, BufferTextureRegion);
			RHICmdList.TransitionBuffers(std::array{
				FRHIBufferTransition{FirstBuffer, 0, 64,
					ERHIAccess::TransferWrite, ERHIAccess::TransferRead},
				FRHIBufferTransition{SecondBuffer, 0, 64,
					ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			RHICmdList.CopyBuffer(FirstBuffer, SecondBuffer,
				std::array{FRHIBufferCopyRegion{0, 0, 32},
					FRHIBufferCopyRegion{32, 32, 32}});
			RHICmdList.TransitionBuffers(std::array{FRHIBufferTransition{
				SecondBuffer, 0, 64, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}});
			RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
				FinalTexture, WholeColor, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			RHICmdList.CopyBufferToTexture(SecondBuffer, FinalTexture, BufferTextureRegion);
			RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
				FinalTexture, WholeColor, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});

			std::vector<uint8> Actual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				RHICmdList, FinalTexture, 0, 0, Actual)) << Mode;
			EXPECT_EQ(Actual, (std::vector<uint8>(Expected.begin(), Expected.end()))) << Mode;

			const FRHITextureCreateDesc CubeDesc = FRHITextureCreateDesc::CreateCube("CopyCube")
				.SetExtent(4)
				.SetNumMips(2)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy |
					ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback);
			FTextureRHIRef SourceCube = GDynamicRHI->RHICreateTexture(RHICmdList, CubeDesc);
			FTextureRHIRef DestinationCube = GDynamicRHI->RHICreateTexture(RHICmdList, CubeDesc);
			ASSERT_TRUE(SourceCube && DestinationCube) << Mode;
			std::array<uint8, 16> CubeMipBytes{};
			for (uint32 Index = 0; Index < CubeMipBytes.size(); ++Index)
				CubeMipBytes[Index] = static_cast<uint8>(0xa0 + Index);
			GDynamicRHI->RHIUpdateTexture2D(RHICmdList, SourceCube, 1, 2,
				FUpdateTextureRegion2D(0, 0, 0, 0, 2, 2), 8, CubeMipBytes.data());
			const FRHITextureSubresourceRange SourceCubeRange{
				ERHITextureAspect::Color, 1, 1, 2, 1};
			const FRHITextureSubresourceRange DestinationCubeRange{
				ERHITextureAspect::Color, 1, 1, 4, 1};
			RHICmdList.TransitionTextures(std::array{
				FRHITextureTransition{SourceCube, SourceCubeRange,
					ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
				FRHITextureTransition{DestinationCube, DestinationCubeRange,
					ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			const std::array CubeCopyRegion{FRHITextureCopyRegion{
				.SourceMip = 1,
				.SourceFirstArrayLayer = 2,
				.DestinationMip = 1,
				.DestinationFirstArrayLayer = 4,
				.Extent = {2, 2, 1}}};
			RHICmdList.CopyTexture(SourceCube, DestinationCube, CubeCopyRegion);
			RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
				DestinationCube, DestinationCubeRange,
				ERHIAccess::TransferWrite, ERHIAccess::ComputeShaderRead}});
			std::vector<uint8> CubeActual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				RHICmdList, DestinationCube, 1, 4, CubeActual)) << Mode;
			EXPECT_EQ(CubeActual,
				(std::vector<uint8>(CubeMipBytes.begin(), CubeMipBytes.end()))) << Mode;

			const FRHITextureCreateDesc CompressedDesc = FRHITextureCreateDesc::Create2D(
				"CompressedCopy", 8, 8, EPixelFormat::BC1_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy |
					ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback);
			FTextureRHIRef CompressedSource = GDynamicRHI->RHICreateTexture(RHICmdList, CompressedDesc);
			FTextureRHIRef CompressedDestination = GDynamicRHI->RHICreateTexture(RHICmdList, CompressedDesc);
			ASSERT_TRUE(CompressedSource && CompressedDestination) << Mode;
			std::array<uint8, 32> CompressedBytes{};
			for (uint32 Index = 0; Index < CompressedBytes.size(); ++Index)
				CompressedBytes[Index] = static_cast<uint8>(Index * 7 + 3);
			GDynamicRHI->RHIUpdateTexture2D(RHICmdList, CompressedSource, 0, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, 8, 8), 16, CompressedBytes.data());
			RHICmdList.TransitionTextures(std::array{
				FRHITextureTransition{CompressedSource, WholeColor,
					ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
				FRHITextureTransition{CompressedDestination, WholeColor,
					ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			RHICmdList.CopyTexture(CompressedSource, CompressedDestination,
				std::array{FRHITextureCopyRegion{.Extent = {8, 8, 1}}});
			RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
				CompressedDestination, WholeColor,
				ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});
			std::vector<uint8> CompressedActual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				RHICmdList, CompressedDestination, 0, 0, CompressedActual)) << Mode;
			EXPECT_EQ(CompressedActual,
				(std::vector<uint8>(CompressedBytes.begin(), CompressedBytes.end()))) << Mode;
			SourceTexture = nullptr;
			MiddleTexture = nullptr;
			FinalTexture = nullptr;
			FirstBuffer = nullptr;
			SecondBuffer = nullptr;
			SourceCube = nullptr;
			DestinationCube = nullptr;
			CompressedSource = nullptr;
			CompressedDestination = nullptr;
			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		}
	}
}
