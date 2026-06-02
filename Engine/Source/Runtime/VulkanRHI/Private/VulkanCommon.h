#pragma once

#include "PixelFormat.h"

namespace Durin::VulkanRHI
{
	namespace EShaderStage
	{
		enum : uint32
		{
			Vertex = 0U,
			Fragment,
			Geometry,
			Mesh,
			Amplification,

			NonComputeStageCount, // used for counting the number of non-compute shader stages

			Compute = NonComputeStageCount,

			Count
		};
	}

} // namespace Durin::VulkanRHI