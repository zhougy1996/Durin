#pragma once

namespace Doge
{
	class FRHIResource;

	struct FRHIShaderParameterResource
	{
		enum class EType
		{
			UniformBuffer,
			Texture,
			Sampler,
		};

		FRHIResource* Resource = nullptr;
		uint16 SetIndex = 0;
		uint16 BindIndex = 0;
		EType Type;
	};
} // namespace Doge