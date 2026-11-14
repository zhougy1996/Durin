#pragma once

#include "Misc/CoreStd.h"
#include "CoreAPI.h"
#include "Misc/FilePath.h"
#include "Hash/XxHash.h"
#include "Misc/FileError.h"

namespace Durin
{
	// Optimistic identity; callers must serialize commits on their owning thread.
	struct FFilePublicationStamp
	{
		bool Exists = false;
		uintmax_t Size = 0;
		std::filesystem::file_time_type Time{};
		auto operator==(const FFilePublicationStamp&) const -> bool = default;
		CORE_API static auto Inspect(const FFilePath& Path, FFilePublicationStamp& Out, std::optional<FFileError>* OutError = nullptr) -> bool;
	};
	// No format knowledge. A replacement retains its backup until explicit finalization.
	// The owning writer coordinates rollback, including failed publication.
	// Multiple file replacements are not crash-atomic.
	struct FFileReplacement
	{
		FFilePath Destination, Staged, Backup;
		bool bBackedUp = false;
		bool bPublished = false;
		CORE_API auto Publish(std::optional<FFileError>* OutError = nullptr) -> bool;
		CORE_API auto Rollback(std::optional<FFileError>* OutError = nullptr) -> bool;
		CORE_API auto Finalize(std::optional<FFileError>* OutError = nullptr) -> bool;
	};
}
