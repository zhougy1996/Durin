#include "EncodedSourceSnapshot.h"

#include "Misc/FileTime.h"
#include "Misc/FileIO.h"
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
		case EEncodedSourceError::Timestamp: return Error.FileError ? Error.FileError->ToString() : "Unable to inspect the source file.";
		case EEncodedSourceError::Limit: return "Encoded source exceeds the configured limit.";
		case EEncodedSourceError::Read: return Error.FileError ? Error.FileError->ToString()
			: std::format("Failed to read source file '{}'.", Error.Filename);
		case EEncodedSourceError::Changed: return Error.FileError ? Error.FileError->ToString()
			: "Source file changed while its snapshot was captured.";
		}
		return {};
	}

	auto CaptureEncodedSource(std::string Filename, const FFilePath& PhysicalPath,
		uint64 MaximumEncodedBytes) -> std::expected<FEncodedSourceSnapshot, FEncodedSourceError>
	{
		FEncodedSourceError Context{.Filename = Filename, .PhysicalPath = PhysicalPath,
			.MaximumEncodedBytes = MaximumEncodedBytes};
		auto Fail = [&](EEncodedSourceError Code, std::error_code SystemError = {},
			FFileIO::EFileOperation Operation = FFileIO::EFileOperation::Inspect) -> std::expected<FEncodedSourceSnapshot, FEncodedSourceError> {
			Context.Code = Code;
			if (SystemError) Context.FileError = FFileIO::FFileError{Operation, SystemError, PhysicalPath};
			return std::unexpected(std::move(Context));
		};
		std::error_code Error;
		const uint64 FileSize = std::filesystem::file_size(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::FileSize, Error, FFileIO::EFileOperation::QuerySize);
		Context.SizeBefore = FileSize;
		if (FileSize > MaximumEncodedBytes || FileSize > static_cast<uint64>(std::numeric_limits<size_t>::max()))
			return Fail(EEncodedSourceError::Limit);
		const auto LastWriteTime = std::filesystem::last_write_time(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::Timestamp, Error);
		auto Loaded = FFileIO::LoadFileToArray(PhysicalPath);
		if (!Loaded)
		{
			Context.FileError = std::move(Loaded.error());
			return Fail(EEncodedSourceError::Read);
		}
		auto Bytes = std::make_shared<FByteBuffer>(std::move(*Loaded));
		Context.BytesRead = Bytes->size();
		Context.SizeAfter = std::filesystem::file_size(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::Changed, Error, FFileIO::EFileOperation::QuerySize);
		const auto TimeAfter = std::filesystem::last_write_time(PhysicalPath, Error);
		if (Error) return Fail(EEncodedSourceError::Changed, Error);
		if (Context.SizeAfter != FileSize || TimeAfter != LastWriteTime || Bytes->size() != FileSize)
			return Fail(EEncodedSourceError::Changed);
		FEncodedSourceSnapshot OutSnapshot{.Filename = std::move(Filename), .PhysicalPath = PhysicalPath,
			.Bytes = std::move(Bytes), .FileSize = FileSize,
			.LastWriteTime = FileTime::ToStableTicks(LastWriteTime)};
		OutSnapshot.ContentHash = FXxHash128::HashBuffer(OutSnapshot.GetBytes());
		return OutSnapshot;
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
