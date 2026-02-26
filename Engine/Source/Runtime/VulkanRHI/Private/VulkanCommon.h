#pragma once

#include "PixelFormat.h"

namespace Doge::VulkanRHI
{
	enum EShaderStage : uint32
	{
		SHADER_STAGE_VERTEX = 0U,
		SHADER_STAGE_PIXEL,
		SHADER_STAGE_GEOMETRY,
		SHADER_STAGE_MESH,
		SHADER_STAGE_AMPLIFICATION,
		NUM_NON_COMPUTE_SHADER_STAGES,
		SHADER_STAGE_COMPUTE = NUM_NON_COMPUTE_SHADER_STAGES,
		NUM_SHADER_STAGES,
	};

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
			return EPixelFormat::UNKNOWN;
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
	}
}