#pragma once

#include "SceneSourceSnapshot.h"
#include "AssetForgeBuiltinsAPI.h"
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

	enum class EEncodedSourceError : uint8 { None, FileSize, Limit, Timestamp, Read, Changed };
	struct FEncodedSourceError
	{
		EEncodedSourceError Code = EEncodedSourceError::None;
		std::string Filename;
		std::filesystem::path PhysicalPath;
		uint64 MaximumEncodedBytes = 0;
		uint64 SizeBefore = 0;
		uint64 SizeAfter = 0;
		uint64 BytesRead = 0;
		std::error_code SystemError;
	};
	struct FEncodedSourceResult
	{
		FEncodedSourceError Error;
		explicit operator bool() const { return Error.Code == EEncodedSourceError::None; }
	};
	ASSETFORGEBUILTINS_API auto FormatEncodedSourceError(const FEncodedSourceError& Error) -> std::string;

	ASSETFORGEBUILTINS_API auto CaptureEncodedSource(
		std::string Filename,
		const std::filesystem::path& PhysicalPath,
		FEncodedSourceSnapshot& OutSnapshot,
		uint64 MaximumEncodedBytes = std::numeric_limits<uint64>::max()) -> FEncodedSourceResult;
	auto UseCapturedSource(
		const FSourceSnapshotEntry& Source,
		FEncodedSourceSnapshot& OutSnapshot) -> void;
}
