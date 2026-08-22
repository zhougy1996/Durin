#pragma once

#include "CoreAPI.h"
#include "Hash/XxHash.h"

namespace Durin
{
	namespace FFileHelper
	{
		// Identifies the filesystem operation that prevented atomic file publication.
		enum class EAtomicFileOperation : uint8
		{
			None,
			NormalizeDestination,
			CreateParentDirectories,
			CreateTemporaryFile,
			WriteTemporaryFile,
			FlushTemporaryFile,
			CloseTemporaryFile,
			ReplaceDestination
		};

		// Preserves the native cause and path metrics for an atomic publication failure.
		struct FAtomicFileError
		{
			EAtomicFileOperation Operation = EAtomicFileOperation::None;
			std::error_code NativeError;
			std::filesystem::path Path;
			size_t PathLength = 0;
			size_t LongestComponentLength = 0;

			CORE_API auto ToString() const -> std::string;
		};

		CORE_API bool FileExists(std::string_view FileName);

		CORE_API bool LoadFileToArray(std::vector<uint8>& Result, const std::filesystem::path& FilePath);

		CORE_API bool LoadFileToArray(std::vector<std::byte>& Result, const std::filesystem::path& FilePath);

		CORE_API bool LoadFileToArray(std::vector<uint32>& Result, std::string_view FileName);

		CORE_API bool LoadFileToString(std::string& Result, std::string_view FileName);

		// Hashes a file incrementally with bounded memory. The caller owns any
		// before/after metadata checks needed for a larger immutable snapshot.
		CORE_API auto HashFileXx128(
			const std::filesystem::path& FilePath,
			FXxHash128& OutHash,
			std::error_code& OutError) -> bool;

		CORE_API bool SaveArrayToFile(const std::span<const std::byte>& Array, const std::filesystem::path& FilePath);

		CORE_API bool SaveArrayToFile(const std::span<const uint32>& Array, const std::filesystem::path& FilePath);

		// Publishes complete bytes through a fixed-length sibling temporary file.
		// Concurrent publishers are last-writer-wins and never expose partial bytes.
		CORE_API auto SaveArrayToFileAtomically(
			std::span<const std::byte> Array,
			const std::filesystem::path& FilePath,
			FAtomicFileError* OutError = nullptr
		) -> bool;

		inline auto SaveArrayToFileAtomically(
			std::span<const uint8> Array,
			const std::filesystem::path& FilePath,
			FAtomicFileError* OutError = nullptr) -> bool
		{
			return SaveArrayToFileAtomically(std::as_bytes(Array), FilePath, OutError);
		}
	}
}
