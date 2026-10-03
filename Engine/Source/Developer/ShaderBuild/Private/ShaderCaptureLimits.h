#pragma once

#include "CoreMinimal.h"

#include "Misc/CoreTypes.h"

namespace Durin::ShaderCaptureLimits
{
	inline constexpr uint64 MaximumMounts = 256;
	inline constexpr uint64 MaximumDirectoryEntries = 131072;
	inline constexpr uint64 MaximumFiles = 65536;
	inline constexpr uint64 MaximumPathBytes = 4096;
	inline constexpr uint64 MaximumFileBytes = 64ull * 1024 * 1024;
	inline constexpr uint64 MaximumTotalBytes = 512ull * 1024 * 1024;
	inline constexpr uint64 MaximumFileTableBytes = 4 + MaximumFiles * (8 + MaximumPathBytes);
	inline constexpr uint64 MaximumSessionInputBytes = MaximumTotalBytes + MaximumFileTableBytes + 1024 * 1024;
}
