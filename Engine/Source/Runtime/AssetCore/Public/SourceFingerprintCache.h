#pragma once

#include "AssetCoreAPI.h"

namespace Durin::Asset
{
	struct FSourceFingerprint
	{
		uint64 FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		std::string ContentHash;
	};

	// Looks up and records hashes already verified for a source file's current
	// cheap filesystem fingerprint. Failures are non-fatal cache misses.
	ASSETCORE_API auto FindSourceFingerprint(
		const std::filesystem::path& SourcePath,
		uint64 FileSize,
		int64 LastWriteTimeTicks,
		std::string& OutContentHash) -> bool;
	ASSETCORE_API auto StoreSourceFingerprint(
		const std::filesystem::path& SourcePath,
		const FSourceFingerprint& Fingerprint) -> bool;
}
