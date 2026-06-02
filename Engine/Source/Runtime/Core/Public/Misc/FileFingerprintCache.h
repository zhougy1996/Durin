#pragma once

#include "CoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/CoreMiscDefines.h"

namespace Durin
{
	struct FFileFingerprint
	{
		std::string NormalizedPath;
		std::filesystem::file_time_type LastWriteTime{};
		uint64 FileSize = 0;
		FXxHash64 ContentHash{};
	};

	class FFileFingerprintCache
	{
	public:
		CORE_API FFileFingerprintCache();
		CORE_API ~FFileFingerprintCache();

		CORE_API auto TryGet(std::string_view FilePath, FFileFingerprint& OutFingerprint, std::string& OutErrorMessage) -> bool;
		CORE_API auto Clear() -> void;

		DURIN_NONCOPYABLE(FFileFingerprintCache);

	private:
		struct FEntry
		{
			std::filesystem::file_time_type LastWriteTime{};
			uint64 FileSize = 0;
			FXxHash64 ContentHash{};
		};

		auto NormalizePath(const std::filesystem::path& InPath) const -> std::string;

		std::mutex Mutex;
		std::unordered_map<std::string, FEntry> Entries;
	};
} // namespace Durin
