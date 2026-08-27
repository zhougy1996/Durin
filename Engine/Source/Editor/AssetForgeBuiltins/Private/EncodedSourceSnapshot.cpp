#include "EncodedSourceSnapshot.h"

#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	auto CaptureEncodedSource(
		std::string Filename,
		const std::filesystem::path& PhysicalPath,
		FEncodedSourceSnapshot& OutSnapshot,
		std::string& OutError,
		uint64 MaximumEncodedBytes) -> bool
	{
		OutSnapshot = {};
		OutError.clear();
		std::error_code Error;
		const uint64 FileSize = std::filesystem::file_size(PhysicalPath, Error);
		if (Error || FileSize > MaximumEncodedBytes
			|| FileSize > static_cast<uint64>(std::numeric_limits<size_t>::max()))
		{
			OutError = Error ? Error.message() : "Encoded source exceeds the configured limit.";
			return false;
		}
		const std::filesystem::file_time_type LastWriteTime =
			std::filesystem::last_write_time(PhysicalPath, Error);
		if (Error)
		{
			OutError = Error.message();
			return false;
		}
		auto Bytes = std::make_shared<std::vector<std::byte>>();
		if (!FFileHelper::LoadFileToArray(*Bytes, PhysicalPath))
		{
			OutError = std::format("Failed to read source file '{}'.", Filename);
			return false;
		}
		const uint64 SizeAfter = std::filesystem::file_size(PhysicalPath, Error);
		const std::filesystem::file_time_type TimeAfter =
			std::filesystem::last_write_time(PhysicalPath, Error);
		if (Error || SizeAfter != FileSize || TimeAfter != LastWriteTime
			|| Bytes->size() != FileSize)
		{
			OutError = "Source file changed while its snapshot was captured.";
			return false;
		}
		OutSnapshot = {
			.Filename = std::move(Filename),
			.PhysicalPath = PhysicalPath,
			.Bytes = std::move(Bytes),
			.FileSize = FileSize,
			.LastWriteTime = FileTime::ToStableTicks(LastWriteTime)};
		OutSnapshot.ContentHash = FXxHash128::HashBuffer(OutSnapshot.GetBytes());
		return true;
	}

	auto UseCapturedSource(
		const FSourceSnapshotEntry& Source,
		FEncodedSourceSnapshot& OutSnapshot) -> void
	{
		OutSnapshot = {
			.Filename = Source.Filename,
			.Bytes = Source.Bytes,
			.ContentHash = Source.ContentHash,
			.FileSize = Source.ByteCount};
	}
}
