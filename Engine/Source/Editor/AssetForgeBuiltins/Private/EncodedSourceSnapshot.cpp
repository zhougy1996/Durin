#include "EncodedSourceSnapshot.h"

#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	auto FormatEncodedSourceError(const FEncodedSourceError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EEncodedSourceError::None: return {};
		case EEncodedSourceError::FileSize:
		case EEncodedSourceError::Timestamp: return Error.SystemError.message();
		case EEncodedSourceError::Limit: return "Encoded source exceeds the configured limit.";
		case EEncodedSourceError::Read: return std::format("Failed to read source file '{}'.", Error.Filename);
		case EEncodedSourceError::Changed: return "Source file changed while its snapshot was captured.";
		}
		return {};
	}

	auto CaptureEncodedSource(std::string Filename, const std::filesystem::path& PhysicalPath,
		FEncodedSourceSnapshot& OutSnapshot, uint64 MaximumEncodedBytes) -> FEncodedSourceResult
	{
		OutSnapshot = {};
		FEncodedSourceError Context{.Filename = Filename, .PhysicalPath = PhysicalPath,
			.MaximumEncodedBytes = MaximumEncodedBytes};
		auto Fail = [&](EEncodedSourceError Code, std::error_code SystemError = {}) -> FEncodedSourceResult {
			Context.Code = Code;
			Context.SystemError = SystemError;
			return {.Error = Context};
		};
		std::error_code Error;
		const uint64 FileSize = std::filesystem::file_size(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::FileSize, Error);
		Context.SizeBefore = FileSize;
		if (FileSize > MaximumEncodedBytes || FileSize > static_cast<uint64>(std::numeric_limits<size_t>::max()))
			return Fail(EEncodedSourceError::Limit);
		const auto LastWriteTime = std::filesystem::last_write_time(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::Timestamp, Error);
		auto Bytes = std::make_shared<FByteBuffer>();
		if (!FFileHelper::LoadFileToArray(*Bytes, PhysicalPath)) return Fail(EEncodedSourceError::Read);
		Context.BytesRead = Bytes->size();
		Context.SizeAfter = std::filesystem::file_size(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::Changed, Error);
		const auto TimeAfter = std::filesystem::last_write_time(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::Changed, Error);
		if (Context.SizeAfter != FileSize || TimeAfter != LastWriteTime || Bytes->size() != FileSize)
			return Fail(EEncodedSourceError::Changed);
		OutSnapshot = {.Filename = std::move(Filename), .PhysicalPath = PhysicalPath,
			.Bytes = std::move(Bytes), .FileSize = FileSize,
			.LastWriteTime = FileTime::ToStableTicks(LastWriteTime)};
		OutSnapshot.ContentHash = FXxHash128::HashBuffer(OutSnapshot.GetBytes());
		return {};
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
