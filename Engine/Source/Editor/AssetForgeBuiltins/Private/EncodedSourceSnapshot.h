#pragma once

#include "SceneSourceSnapshot.h"
#include "Hash/XxHash.h"

namespace Durin::AssetForge::Builtins
{
	// Owns one immutable source capture used consistently for hashing, decoding, and build composition.
	struct FEncodedSourceSnapshot
	{
		std::string Filename;
		std::filesystem::path PhysicalPath;
		std::shared_ptr<const FByteBuffer> Bytes;
		FXxHash128 ContentHash{};
		uint64 FileSize = 0;
		int64 LastWriteTime = 0;

		auto GetBytes() const -> FByteView
		{
			return Bytes ? FByteView(*Bytes) : FByteView{};
		}
	};

	auto CaptureEncodedSource(
		std::string Filename,
		const std::filesystem::path& PhysicalPath,
		FEncodedSourceSnapshot& OutSnapshot,
		std::string& OutError,
		uint64 MaximumEncodedBytes = std::numeric_limits<uint64>::max()) -> bool;
	auto UseCapturedSource(
		const FSourceSnapshotEntry& Source,
		FEncodedSourceSnapshot& OutSnapshot) -> void;
}
