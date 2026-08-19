#pragma once

#include "Asset/MountedSource.h"
#include "AssetImportCore.h"
#include "Hash/XxHash.h"

namespace Durin::Asset::Forge
{
	// Owns one immutable source capture used consistently for hashing, decoding, and build composition.
	struct FEncodedSourceSnapshot
	{
		FSourcePath SourcePath;
		std::filesystem::path PhysicalPath;
		std::shared_ptr<const std::vector<uint8>> Bytes;
		FXxHash128 ContentHash{};
		uint64 FileSize = 0;
		int64 LastWriteTime = 0;

		auto GetBytes() const -> std::span<const uint8>
		{
			return Bytes ? std::span<const uint8>(*Bytes) : std::span<const uint8>{};
		}
	};

	auto CaptureEncodedSource(
		const FMountedSourceFile& Source,
		FEncodedSourceSnapshot& OutSnapshot,
		std::string& OutError,
		uint64 MaximumEncodedBytes = std::numeric_limits<uint64>::max()) -> bool;
	auto CaptureEncodedSource(
		const FSourcePath& SourcePath,
		FEncodedSourceSnapshot& OutSnapshot,
		std::string& OutError,
		uint64 MaximumEncodedBytes = std::numeric_limits<uint64>::max()) -> bool;
	auto UseCapturedSource(
		const FSourceSnapshotEntry& Source,
		FEncodedSourceSnapshot& OutSnapshot) -> void;
}
