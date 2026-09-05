#include "Misc/FileFingerprintCache.h"

#include "Misc/FileHelper.h"

namespace Durin
{
	FFileFingerprintCache::FFileFingerprintCache() = default;
	FFileFingerprintCache::~FFileFingerprintCache() = default;

	auto FFileFingerprintCache::NormalizePath(const std::filesystem::path& InPath) const -> std::string
	{
		std::error_code ErrorCode;
		const std::filesystem::path CanonicalPath = std::filesystem::weakly_canonical(InPath, ErrorCode);
		if (!ErrorCode)
		{
			return CanonicalPath.generic_string();
		}
		return InPath.lexically_normal().generic_string();
	}

	auto FFileFingerprintCache::TryGet(std::string_view FilePath, FFileFingerprint& OutFingerprint, std::string& OutErrorMessage) -> bool
	{
		const std::string NormalizedPath = NormalizePath(std::filesystem::path(std::string(FilePath)));

		std::error_code ErrorCode;
		if (!std::filesystem::exists(NormalizedPath, ErrorCode))
		{
			OutErrorMessage = ErrorCode
				? std::format("Failed to stat file {}: {}", NormalizedPath, ErrorCode.message())
				: std::format("File does not exist: {}", NormalizedPath);
			return false;
		}

		const std::filesystem::file_time_type LastWriteTime = std::filesystem::last_write_time(NormalizedPath, ErrorCode);
		if (ErrorCode)
		{
			OutErrorMessage = std::format("Failed to query file timestamp {}: {}", NormalizedPath, ErrorCode.message());
			return false;
		}

		const uint64 FileSize = std::filesystem::file_size(NormalizedPath, ErrorCode);
		if (ErrorCode)
		{
			OutErrorMessage = std::format("Failed to query file size {}: {}", NormalizedPath, ErrorCode.message());
			return false;
		}

		{
			std::lock_guard Lock(Mutex);
			if (const auto FoundIt = Entries.find(NormalizedPath); FoundIt != Entries.end())
			{
				const FEntry& CachedFingerprint = FoundIt->second;
				if (CachedFingerprint.LastWriteTime == LastWriteTime && CachedFingerprint.FileSize == FileSize)
				{
					OutFingerprint.NormalizedPath = NormalizedPath;
					OutFingerprint.LastWriteTime = CachedFingerprint.LastWriteTime;
					OutFingerprint.FileSize = CachedFingerprint.FileSize;
					OutFingerprint.ContentHash = CachedFingerprint.ContentHash;
					return true;
				}
			}
		}

		FByteArray FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, NormalizedPath))
		{
			OutErrorMessage = std::format("Failed to read file: {}", NormalizedPath);
			return false;
		}

		FEntry NewEntry;
		NewEntry.LastWriteTime = LastWriteTime;
		NewEntry.FileSize = FileSize;
		NewEntry.ContentHash = FXxHash64::HashBuffer(std::span<const std::byte>(FileBytes));

		{
			std::lock_guard Lock(Mutex);
			Entries.insert_or_assign(NormalizedPath, NewEntry);
			++ContentReadCount;
		}

		OutFingerprint.NormalizedPath = NormalizedPath;
		OutFingerprint.LastWriteTime = NewEntry.LastWriteTime;
		OutFingerprint.FileSize = NewEntry.FileSize;
		OutFingerprint.ContentHash = NewEntry.ContentHash;
		return true;
	}

	auto FFileFingerprintCache::TryReuse(const FFileFingerprint& StoredFingerprint)
		-> FFileFingerprintReuseResult
	{
		const std::string NormalizedPath = NormalizePath(std::filesystem::path(StoredFingerprint.NormalizedPath));
		if (NormalizedPath != StoredFingerprint.NormalizedPath)
		{
			return {.Status = EFileFingerprintReuseStatus::Stale};
		}

		std::error_code ErrorCode;
		if (!std::filesystem::exists(NormalizedPath, ErrorCode))
		{
			if (ErrorCode)
			{
				return {.Status = EFileFingerprintReuseStatus::Failed,
					.Diagnostic = std::format(
						"Failed to stat file {}: {}", NormalizedPath, ErrorCode.message())};
			}
			return {.Status = EFileFingerprintReuseStatus::Stale};
		}

		const std::filesystem::file_time_type LastWriteTime = std::filesystem::last_write_time(NormalizedPath, ErrorCode);
		if (ErrorCode)
		{
			return {.Status = EFileFingerprintReuseStatus::Failed,
				.Diagnostic = std::format(
					"Failed to query file timestamp {}: {}", NormalizedPath, ErrorCode.message())};
		}

		const uint64 FileSize = std::filesystem::file_size(NormalizedPath, ErrorCode);
		if (ErrorCode)
		{
			return {.Status = EFileFingerprintReuseStatus::Failed,
				.Diagnostic = std::format(
					"Failed to query file size {}: {}", NormalizedPath, ErrorCode.message())};
		}

		if (LastWriteTime != StoredFingerprint.LastWriteTime || FileSize != StoredFingerprint.FileSize)
		{
			return {.Status = EFileFingerprintReuseStatus::Stale};
		}

		{
			std::lock_guard Lock(Mutex);
			Entries.insert_or_assign(NormalizedPath, FEntry{
				.LastWriteTime = StoredFingerprint.LastWriteTime,
				.FileSize = StoredFingerprint.FileSize,
				.ContentHash = StoredFingerprint.ContentHash
			});
		}
		return {.Status = EFileFingerprintReuseStatus::Current};
	}

	auto FFileFingerprintCache::GetContentReadCount() const -> uint64
	{
		std::lock_guard Lock(Mutex);
		return ContentReadCount;
	}

	auto FFileFingerprintCache::Clear() -> void
	{
		std::lock_guard Lock(Mutex);
		Entries.clear();
		ContentReadCount = 0;
	}
} // namespace Durin
