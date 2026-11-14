#include "AssetPackageFingerprintInternal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "AssetPackageCodec.h"
#include "Misc/FileTime.h"

namespace Durin::AssetPrivate
{
	namespace
	{
		auto Error(Durin::UnifiedError::FErrorId Id, std::string Message) -> std::expected<void, Durin::UnifiedError::FError>
		{ return std::unexpected(Durin::UnifiedError::FError{Id, std::move(Message)}); }
	}

	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> std::expected<void, Durin::UnifiedError::FError>
	{
		std::error_code ErrorCode;
		const std::filesystem::path Path(PhysicalPath);
		const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
		if (ErrorCode)
			return Error(Durin::PackageReadErrors::IoError, std::format(
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
