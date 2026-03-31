#pragma once

#include "PixelFormat.h"

namespace Doge::VulkanRHI
{

	namespace EShaderStage
	{
		enum : uint32
		{
			Vertex = 0U,
			Pixel,
			Geometry,
			Mesh,
			Amplification,

			NonComputeStageCount, // used for counting the number of non-compute shader stages

			Compute = NonComputeStageCount,

			Count
		};
	}

	namespace FVulkanPixelFormat
	{
		inline EPixelFormat ToPixelFormat(vk::Format InFormat)
		{
			switch (InFormat)
			{
			case vk::Format::eR8G8B8A8Srgb:
				return EPixelFormat::SRGBA8_UNORM;
			case vk::Format::eB8G8R8A8Srgb:
				return EPixelFormat::SBGRA8_UNORM;
			default:
				DOGE_ERROR("Unsupported pixel format {}", vk::to_string(InFormat));
			}
			return EPixelFormat::Unknown;
		}

		inline vk::Format FromPixelFormat(EPixelFormat InFormat)
		{
			switch (InFormat)
			{
			case EPixelFormat::SRGBA8_UNORM:
				return vk::Format::eR8G8B8A8Srgb;
			case EPixelFormat::SBGRA8_UNORM:
				return vk::Format::eB8G8R8A8Srgb;
			default:
				DOGE_ERROR("Unsupported pixel format {}", static_cast<uint32>(InFormat));
			}
			return vk::Format::eUndefined;
		}
	} // namespace FVulkanPixelFormat
} // namespace Doge::VulkanRHI