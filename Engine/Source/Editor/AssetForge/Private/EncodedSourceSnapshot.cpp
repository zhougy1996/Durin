#include "EncodedSourceSnapshot.h"

#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset::Forge
{
	auto CaptureEncodedSource(
		const FMountedSourceFile& Source,
		FEncodedSourceSnapshot& OutSnapshot,
		std::string& OutError,
		uint64 MaximumEncodedBytes) -> bool
	{
		OutSnapshot = {};
		OutError.clear();
		std::error_code Error;
		const uint64 FileSize = std::filesystem::file_size(Source.PhysicalPath, Error);
		if (Error || FileSize > MaximumEncodedBytes
			|| FileSize > static_cast<uint64>(std::numeric_limits<size_t>::max()))
		{
			OutError = Error ? Error.message() : "Encoded source exceeds the configured limit.";
			return false;
		}
		const std::filesystem::file_time_type LastWriteTime =
			std::filesystem::last_write_time(Source.PhysicalPath, Error);
		if (Error)
		{
			OutError = Error.message();
			return false;
		}
		auto Bytes = std::make_shared<std::vector<uint8>>();
		if (!FFileHelper::LoadFileToArray(*Bytes, Source.PhysicalPath.generic_string()))
		{
			OutError = std::format("Failed to read mounted source '{}'.", Source.SourcePath.Path);
			return false;
		}
		const uint64 SizeAfter = std::filesystem::file_size(Source.PhysicalPath, Error);
		const std::filesystem::file_time_type TimeAfter =
			std::filesystem::last_write_time(Source.PhysicalPath, Error);
		if (Error || SizeAfter != FileSize || TimeAfter != LastWriteTime
			|| Bytes->size() != FileSize)
		{
			OutError = "Mounted source changed while its snapshot was captured.";
			return false;
		}
		OutSnapshot = {
			.SourcePath = Source.SourcePath,
			.PhysicalPath = Source.PhysicalPath,
			.Bytes = std::move(Bytes),
			.FileSize = FileSize,
			.LastWriteTime = FileTime::ToStableTicks(LastWriteTime)};
		OutSnapshot.ContentHash = FXxHash128::HashBuffer(OutSnapshot.GetBytes());
		return true;
	}

	auto CaptureEncodedSource(
		const FSourcePath& SourcePath,
		FEncodedSourceSnapshot& OutSnapshot,
		std::string& OutError,
		uint64 MaximumEncodedBytes) -> bool
	{
		const PathUtilities::FSourcePathResult Resolved = PathUtilities::ResolveSourcePath(
			SourcePath.Path, PathUtilities::EPathExistence::RequireFile);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		return CaptureEncodedSource({.SourcePath = {
			.Path = Resolved.NormalizedVirtualPath}, .PhysicalPath = Resolved.PhysicalPath},
			OutSnapshot, OutError, MaximumEncodedBytes);
	}

	auto UseCapturedSource(
		const FSourceSnapshotEntry& Source,
		FEncodedSourceSnapshot& OutSnapshot) -> void
	{
		OutSnapshot = {
			.SourcePath = Source.SourcePath,
			.Bytes = Source.Bytes,
			.ContentHash = Source.ContentHash,
			.FileSize = Source.ByteCount};
	}
}
