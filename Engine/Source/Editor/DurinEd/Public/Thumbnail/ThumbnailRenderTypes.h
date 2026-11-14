#pragma once

#include "CoreMinimal.h"
#include "SceneView.h"

namespace Durin
{
	// Describes a renderer-selected camera without exposing concrete asset types.
	struct FThumbnailPreviewView
	{
		std::array<double, 3> CameraPosition{};
		std::array<double, 3> CameraForward{1.0, 0.0, 0.0};
		std::array<double, 3> CameraRight{0.0, 1.0, 0.0};
		std::array<double, 3> CameraUp{0.0, 0.0, 1.0};
		double VerticalFieldOfViewDegrees = 42.0;
		double NearClipDistance = 0.1;
		double FarClipDistance = 100.0;
		bool bForceLOD0 = false;
		float ClearRed = 0.0f;
		float ClearGreen = 0.0f;
		float ClearBlue = 0.0f;
		float ClearAlpha = 0.0f;
	};

	// Supplies capture output and view setup without owning renderer scenes.
	// Optional image renderers return SRGBA8 under the same capture/readback budget as scenes.
	using FThumbnailImageRenderer = std::function<FTextureRHIRef(
		FRHICommandListImmediate&, uint32, uint32)>;

} // namespace Durin
