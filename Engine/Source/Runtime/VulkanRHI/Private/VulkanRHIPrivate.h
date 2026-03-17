#pragma once

#include "PixelFormat.h"

namespace Doge::VulkanRHI
{
	 auto ConvertToVulkanFormat(EPixelFormat InFormat) -> vk::Format;

}