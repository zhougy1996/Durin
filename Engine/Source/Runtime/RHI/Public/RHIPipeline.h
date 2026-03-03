#pragma once

namespace Doge
{
	enum class ERHIPipeline : uint8
	{
		Graphics = 1 << 0,
		Compute = 1 << 1,

		None = 0,
		All = Graphics | Compute,
		Num = 2
	};

	class RHI_API FRHIGraphicsPipelineState{
	};
}