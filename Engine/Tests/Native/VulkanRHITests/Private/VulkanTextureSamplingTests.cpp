#include <gtest/gtest.h>

#include "PCH.VulkanRHI.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "Shader/SlangShaderCompiler.h"
#include "Texture/TextureBuild.h"
#include "VulkanDynamicRHI.h"
#include "VulkanTexture.h"

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
				.SetFormat(Format);
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
				.SetFormat(PlatformData.PixelFormat);
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

	TEST(FVulkanTextureSamplingTests, UploadsAndSamplesEveryMipWithKnownColorSpaceValues)
	{
		RHIInit();
		ASSERT_NE(GDynamicRHI, nullptr);
		auto* VulkanRHI = static_cast<VulkanRHI::IVulkanDynamicRHI*>(GDynamicRHI);
		const VkDevice Device = VulkanRHI->RHIGetVkDevice();
		const VkPhysicalDevice PhysicalDevice = VulkanRHI->RHIGetVkPhysicalDevice();
		ASSERT_NE(Device, VK_NULL_HANDLE);
		ASSERT_NE(PhysicalDevice, VK_NULL_HANDLE);

		GDynamicRHI->RHIBeginFrame();
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
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

		const auto* VulkanLinearTexture = static_cast<VulkanRHI::FVulkanTexture*>(LinearTexture.GetReference());
		const auto* VulkanSrgbTexture = static_cast<VulkanRHI::FVulkanTexture*>(SrgbTexture.GetReference());
		const auto* VulkanBc1Texture = static_cast<VulkanRHI::FVulkanTexture*>(Bc1Texture.GetReference());
		const auto* VulkanBc3Texture = static_cast<VulkanRHI::FVulkanTexture*>(Bc3Texture.GetReference());
		const auto* VulkanBc5Texture = static_cast<VulkanRHI::FVulkanTexture*>(Bc5Texture.GetReference());
		const auto* VulkanBc7Texture = static_cast<VulkanRHI::FVulkanTexture*>(Bc7Texture.GetReference());
		const std::array<VkDescriptorImageInfo, TestTextureCount> ImageInfos{{
			{VK_NULL_HANDLE, VulkanLinearTexture->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{VK_NULL_HANDLE, VulkanSrgbTexture->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{VK_NULL_HANDLE, VulkanBc1Texture->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{VK_NULL_HANDLE, VulkanBc3Texture->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{VK_NULL_HANDLE, VulkanBc5Texture->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{VK_NULL_HANDLE, VulkanBc7Texture->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
		}};
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

		const VkCommandBuffer CommandBuffer = VulkanRHI->RHIGetVkCommandBuffer(RHICmdList);
		vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, Pipeline);
		vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout, 0, 1, &DescriptorSet, 0, nullptr);
		vkCmdDispatch(CommandBuffer, 1, 1, 1);
		const VkMemoryBarrier HostBarrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT
		};
		vkCmdPipelineBarrier(
			CommandBuffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT,
			0,
			1,
			&HostBarrier,
			0,
			nullptr,
			0,
			nullptr
		);

		GDynamicRHI->RHIEndFrame();
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
		LinearTexture = nullptr;
		SrgbTexture = nullptr;
		Bc1Texture = nullptr;
		Bc3Texture = nullptr;
		Bc5Texture = nullptr;
		Bc7Texture = nullptr;
		RHICmdList.SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}
}
