#pragma once

#include "Hash/XxHash.h"

namespace Durin::Asset
{
	struct FAssetPackageFingerprint
	{
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		FXxHash128 ContentHash;
		uint32 ReaderVersion = 0;

		auto operator==(const FAssetPackageFingerprint&) const -> bool = default;
	};
}
