#include "AssetPackageFingerprintInternal.h"
#include "AssetPackageCodec.h"
#include "Misc/FileTime.h"

namespace Durin::AssetPrivate
{
	namespace
	{
		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult
		{ return {Code, std::move(Message)}; }
	}

	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult
	{
		std::error_code ErrorCode;
		const std::filesystem::path Path(PhysicalPath);
		const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
		if (ErrorCode)
			return Error(EAssetReadError::IoError, std::format(
				"Failed to read the last-write time for asset package {}.", PhysicalPath));
		uint32 ReaderVersion = 0;
		if (Path.extension() == ".dasset")
		{
			const FAssetPackageCodec* Codec = nullptr;
			const auto ResolveResult = ResolveAssetPackageReader(
				Bytes, Codec, &ReaderVersion);
			if (!ResolveResult) return ResolveResult;
		}
		OutFingerprint = {
			.FileSize = Bytes.size(),
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime),
			.ContentHash = FXxHash128::HashBuffer(Bytes),
			.ReaderVersion = ReaderVersion};
		return {};
	}

}
