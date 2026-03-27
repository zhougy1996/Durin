#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	struct FVulkanFormatMapping
	{
		EPixelFormat RhiFormat;
		vk::Format VulkanFormat;
	};

	static constexpr std::array<FVulkanFormatMapping, static_cast<size_t>(EPixelFormat::Count)> VulkanFormatMap = {{
		{EPixelFormat::Unknown, vk::Format::eUndefined},
		{EPixelFormat::R8_UINT, vk::Format::eR8Uint},
		{EPixelFormat::R8_SINT, vk::Format::eR8Sint},
		{EPixelFormat::R8_UNORM, vk::Format::eR8Unorm},
		{EPixelFormat::R8_SNORM, vk::Format::eR8Snorm},
		{EPixelFormat::RG8_UINT, vk::Format::eR8G8Uint},
		{EPixelFormat::RG8_SINT, vk::Format::eR8G8Sint},
		{EPixelFormat::RG8_UNORM, vk::Format::eR8G8Unorm},
		{EPixelFormat::RG8_SNORM, vk::Format::eR8G8Snorm},
		{EPixelFormat::R16_UINT, vk::Format::eR16Uint},
		{EPixelFormat::R16_SINT, vk::Format::eR16Sint},
		{EPixelFormat::R16_UNORM, vk::Format::eR16Unorm},
		{EPixelFormat::R16_SNORM, vk::Format::eR16Snorm},
		{EPixelFormat::R16_FLOAT, vk::Format::eR16Sfloat},
		{EPixelFormat::BGRA4_UNORM, vk::Format::eA4R4G4B4UnormPack16}, // this format matches the bit layout of DXGI_FORMAT_B4G4R4A4_UNORM
		{EPixelFormat::B5G6R5_UNORM, vk::Format::eB5G6R5UnormPack16},
		{EPixelFormat::B5G5R5A1_UNORM, vk::Format::eB5G5R5A1UnormPack16},
		{EPixelFormat::RGBA8_UINT, vk::Format::eR8G8B8A8Uint},
		{EPixelFormat::RGBA8_SINT, vk::Format::eR8G8B8A8Sint},
		{EPixelFormat::RGBA8_UNORM, vk::Format::eR8G8B8A8Unorm},
		{EPixelFormat::RGBA8_SNORM, vk::Format::eR8G8B8A8Snorm},
		{EPixelFormat::BGRA8_UNORM, vk::Format::eB8G8R8A8Unorm},
		{EPixelFormat::BGRX8_UNORM, vk::Format::eUndefined}, // Not supported on Vulkan
		{EPixelFormat::SRGBA8_UNORM, vk::Format::eR8G8B8A8Srgb},
		{EPixelFormat::SBGRA8_UNORM, vk::Format::eB8G8R8A8Srgb},
		{EPixelFormat::SBGRX8_UNORM, vk::Format::eUndefined}, // Not supported on Vulkan
		{EPixelFormat::R10G10B10A2_UNORM, vk::Format::eA2B10G10R10UnormPack32},
		{EPixelFormat::R11G11B10_FLOAT, vk::Format::eB10G11R11UfloatPack32},
		{EPixelFormat::RG16_UINT, vk::Format::eR16G16Uint},
		{EPixelFormat::RG16_SINT, vk::Format::eR16G16Sint},
		{EPixelFormat::RG16_UNORM, vk::Format::eR16G16Unorm},
		{EPixelFormat::RG16_SNORM, vk::Format::eR16G16Snorm},
		{EPixelFormat::RG16_FLOAT, vk::Format::eR16G16Sfloat},
		{EPixelFormat::R32_UINT, vk::Format::eR32Uint},
		{EPixelFormat::R32_SINT, vk::Format::eR32Sint},
		{EPixelFormat::R32_FLOAT, vk::Format::eR32Sfloat},
		{EPixelFormat::RGBA16_UINT, vk::Format::eR16G16B16A16Uint},
		{EPixelFormat::RGBA16_SINT, vk::Format::eR16G16B16A16Sint},
		{EPixelFormat::RGBA16_FLOAT, vk::Format::eR16G16B16A16Sfloat},
		{EPixelFormat::RGBA16_UNORM, vk::Format::eR16G16B16A16Unorm},
		{EPixelFormat::RGBA16_SNORM, vk::Format::eR16G16B16A16Snorm},
		{EPixelFormat::RG32_UINT, vk::Format::eR32G32Uint},
		{EPixelFormat::RG32_SINT, vk::Format::eR32G32Sint},
		{EPixelFormat::RG32_FLOAT, vk::Format::eR32G32Sfloat},
		{EPixelFormat::RGB32_UINT, vk::Format::eR32G32B32Uint},
		{EPixelFormat::RGB32_SINT, vk::Format::eR32G32B32Sint},
		{EPixelFormat::RGB32_FLOAT, vk::Format::eR32G32B32Sfloat},
		{EPixelFormat::RGBA32_UINT, vk::Format::eR32G32B32A32Uint},
		{EPixelFormat::RGBA32_SINT, vk::Format::eR32G32B32A32Sint},
		{EPixelFormat::RGBA32_FLOAT, vk::Format::eR32G32B32A32Sfloat},
		{EPixelFormat::D16, vk::Format::eD16Unorm},
		{EPixelFormat::D24S8, vk::Format::eD24UnormS8Uint},
		{EPixelFormat::X24G8_UINT, vk::Format::eD24UnormS8Uint},
		{EPixelFormat::D32, vk::Format::eD32Sfloat},
		{EPixelFormat::D32S8, vk::Format::eD32SfloatS8Uint},
		{EPixelFormat::X32G8_UINT, vk::Format::eD32SfloatS8Uint},
		{EPixelFormat::BC1_UNORM, vk::Format::eBc1RgbaUnormBlock},
		{EPixelFormat::BC1_UNORM_SRGB, vk::Format::eBc1RgbaSrgbBlock},
		{EPixelFormat::BC2_UNORM, vk::Format::eBc2UnormBlock},
		{EPixelFormat::BC2_UNORM_SRGB, vk::Format::eBc2SrgbBlock},
		{EPixelFormat::BC3_UNORM, vk::Format::eBc3UnormBlock},
		{EPixelFormat::BC3_UNORM_SRGB, vk::Format::eBc3SrgbBlock},
		{EPixelFormat::BC4_UNORM, vk::Format::eBc4UnormBlock},
		{EPixelFormat::BC4_SNORM, vk::Format::eBc4SnormBlock},
		{EPixelFormat::BC5_UNORM, vk::Format::eBc5UnormBlock},
		{EPixelFormat::BC5_SNORM, vk::Format::eBc5SnormBlock},
		{EPixelFormat::BC6H_UFLOAT, vk::Format::eBc6HUfloatBlock},
		{EPixelFormat::BC6H_SFLOAT, vk::Format::eBc6HSfloatBlock},
		{EPixelFormat::BC7_UNORM, vk::Format::eBc7UnormBlock},
		{EPixelFormat::BC7_UNORM_SRGB, vk::Format::eBc7SrgbBlock},
	}};

	auto ConvertToVulkanFormat(EPixelFormat InFormat) -> vk::Format
	{
		check(InFormat < EPixelFormat::Count);
		check(VulkanFormatMap[static_cast<uint32>(InFormat)].RhiFormat == InFormat);
		return VulkanFormatMap[static_cast<uint32>(InFormat)].VulkanFormat;
	}

	auto ConvertToVulkanBufferUsageFlags(EBufferUsageFlags InUsage) -> vk::BufferUsageFlags
	{
		switch (InUsage)
		{
		case EBufferUsageFlags::VertexBuffer:		return vk::BufferUsageFlagBits::eVertexBuffer;
		case EBufferUsageFlags::IndexBuffer:		return vk::BufferUsageFlagBits::eIndexBuffer;
		case EBufferUsageFlags::UniformBuffer:		return vk::BufferUsageFlagBits::eUniformBuffer;
		case EBufferUsageFlags::StructuredBuffer:	return vk::BufferUsageFlagBits::eStorageBuffer;
		case EBufferUsageFlags::ByteAddressBuffer:	return vk::BufferUsageFlagBits::eStorageBuffer; // Vulkan doesn't have a specific flag for byte address buffers, but we can treat them as storage buffers
		case EBufferUsageFlags::DrawIndirect:		return vk::BufferUsageFlagBits::eIndirectBuffer;
		case EBufferUsageFlags::ShaderResource:		return vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer; // Depending on the buffer type, it could be either uniform texel buffer or storage texel buffer. The RHI should ensure the correct usage is set
		case EBufferUsageFlags::KeepCPUAccessible:	return vk::BufferUsageFlagBits::eTransferSrc; // This flag is a hint for the buffer to be CPU accessible, we can use it as a transfer source for staging buffers.
		default:
			DOGE_ERROR("Unsupported buffer usage flag: {}", static_cast<uint32>(InUsage));
			return vk::BufferUsageFlags();
		}
	}

	std::atomic<uint64> GVulkanBufferHandleIdCounter = 0;
	std::atomic<uint64> GVulkanBufferViewHandleIdCounter = 0;
	std::atomic<uint64> GVulkanImageViewHandleIdCounter = 0;
	std::atomic<uint64> GVulkanSamplerHandleIdCounter = 0;
	std::atomic<uint64> GVulkanDSetLayoutHandleIdCounter = 0;
} // namespace Doge::VulkanRHI
