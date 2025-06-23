#pragma once

#include "RHIFeatureLevel.h"

class FRenderResource
{
	/** Controls initialization order of render resources. Early engine resources utilize the 'Pre' phase to avoid static init ordering issues. */
	enum class EInitPhase : uint8
	{
		Pre,
		Default,
		MAX
	};

	/** Release all render resources that are currently initialized. */
	static RENDER_CORE_API void ReleaseRHIForAllResources();

	static RENDER_CORE_API void InitPreRHIResources();

	RENDER_CORE_API FRenderResource();
};