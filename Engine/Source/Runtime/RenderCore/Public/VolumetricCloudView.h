#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	// Selects one shipped Renderer-owned volumetric-cloud work policy per view.
	enum class EVolumetricCloudQuality : uint8
	{
		Performance,
		High,
		Epic,
		Reference,
		Count,
	};

	// Development presentation for one view. These modes never mutate authored content.
	enum class EVolumetricCloudDebugMode : uint8
	{
		Lit,
		Radiance,
		Transmittance,
		TemporalStatus,
		ShadowVisibility,
		Count,
	};

	enum class EVolumetricCloudExecutionRoute : uint8
	{
		None,
		Compute,
		Fragment,
	};

	// Stable editor-facing reduction of Renderer-private route diagnostics.
	enum class EVolumetricCloudRouteReason : uint8
	{
		DisabledOrUnneeded,
		InvalidInputs,
		InvalidExtent,
		Compute,
		ComputePayloadUnavailable,
		ComputeTargetUnavailable,
		ComputeExtentUnsupported,
		FragmentPayloadUnavailable,
		FragmentTargetUnavailable,
		Unknown,
	};
}
