#pragma once

#include "CoreMinimal.h"

namespace Durin::MonaImGui
{
	// Preserves element alignment and retains headroom without overflowing RHI's byte size.
	inline auto ComputeImGuiBufferCapacity(uint32 CurrentBytes, uint32 RequiredBytes,
		uint32 MinimumBytes, uint32 ElementSize) -> uint32
	{
		require(ElementSize > 0 && RequiredBytes % ElementSize == 0);
		if (CurrentBytes >= RequiredBytes) return CurrentBytes;
		const uint64 MaximumElements = std::numeric_limits<uint32>::max() / ElementSize;
		const uint64 RequiredElements = RequiredBytes / ElementSize;
		const uint64 BaseElements = std::max<uint64>(CurrentBytes / ElementSize, RequiredElements);
		const uint64 MinimumElements = (static_cast<uint64>(MinimumBytes) + ElementSize - 1) / ElementSize;
		const uint64 CapacityElements = std::min(MaximumElements,
			std::max(MinimumElements, BaseElements + BaseElements / 2));
		return static_cast<uint32>(CapacityElements * ElementSize);
	}
}
