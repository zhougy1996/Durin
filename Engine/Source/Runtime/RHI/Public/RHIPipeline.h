#pragma once

enum class ERHIPipeline : uint8
{
	eGraphics = 1 << 0,
	eCompute = 1 << 1,

	eNone = 0,
	eAll = eGraphics | eCompute,
	Num = 2
};

class RHI_API FRHIGraphicsPipelineState{

};
