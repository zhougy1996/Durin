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

} // namespace Doge::VulkanRHI