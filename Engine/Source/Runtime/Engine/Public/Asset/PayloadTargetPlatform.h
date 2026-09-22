#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	// Identifies a disk-compatible asset build target independently of host platform enums.
	enum class EAssetPayloadTargetPlatform : uint32
	{
		Unknown = 0,
		Win64 = 1
	};

}
