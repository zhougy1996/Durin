#pragma once

#include "RHIDefinitions.h"
#include "RHIFeatureLevel.h"

namespace Durin
{
	// Immutable backend-neutral rendering choices published after successful RHI startup.
	struct FRHICapabilities
	{
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::ES3_1;
		ERHITextureDimensionFlags SupportedTextureDimensions = ERHITextureDimensionFlags::None;
		uint32 MaxTextureDimension2D = 0;
		uint32 MaxTextureDimensionCube = 0;
		uint32 MaxTextureArrayLayers = 0;
		ERHISampleCountFlags ColorSampleCounts = ERHISampleCountFlags::None;
		ERHISampleCountFlags DepthSampleCounts = ERHISampleCountFlags::None;
		bool bSupportsSynchronization2 = false;
	};
}
