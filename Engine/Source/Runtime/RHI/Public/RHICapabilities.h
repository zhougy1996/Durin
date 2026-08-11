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
		uint32 MaxColorAttachments = 0;
		uint32 MinStorageBufferOffsetAlignment = 0;
		uint32 MaxStorageBufferRange = 0;
		std::array<uint32, 3> MaxComputeWorkGroupCount = {};
		bool bSupportsNonSolidFill = false;
		bool bSupportsDepthClamp = false;
		bool bSupportsWideLines = false;
		bool bSupportsSynchronization2 = false;
		bool bSupportsGPUTimestamps = false;
		double GPUTimestampNanosecondsPerTick = 0.0;
	};
}
