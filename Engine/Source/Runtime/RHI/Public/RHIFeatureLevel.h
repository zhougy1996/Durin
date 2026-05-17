#pragma once

namespace Durin
{
	enum class ERHIFeatureLevel : uint8
	{
		/** Feature level defined by the core capabilities of OpenGL ES3.1 & Metal/Vulkan. */
		ES3_1,

		/**
		 * Feature level defined by the capabilities of DX11 Shader Model 5.
		 *   Compute shaders with shared memory, group sync, UAV writes, integer atomics
		 *   Indirect drawing
		 *   Pixel shaders with UAV writes
		 *   Cubemap arrays
		 *   Read-only depth or stencil views (eg read depth buffer as SRV while depth test and stencil write)
		 * Tessellation is not considered part of Feature Level SM5 and has a separate capability flag.
		 */
		SM5,

		/**
		 * Feature level defined by the capabilities of DirectX 12 hardware feature level 12_2 with Shader Model 6.5
		 *   Raytracing Tier 1.1
		 *   Mesh and Amplification shaders
		 *   Variable rate shading
		 *   Sampler feedback
		 *   Resource binding tier 3
		 */
		SM6,
	};
}