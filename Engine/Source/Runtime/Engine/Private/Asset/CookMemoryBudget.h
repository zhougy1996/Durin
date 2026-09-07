#pragma once

namespace Durin::AssetPrivate
{
	// Account retained payloads before accepting them; rejection leaves the total intact.
	inline auto TryRetainCookBytes(uint64 Bytes, uint64& RetainedBytes, uint64 Limit) -> bool
	{
		if (RetainedBytes > Limit || Bytes > Limit - RetainedBytes) return false;
		RetainedBytes += Bytes;
		return true;
	}
}
