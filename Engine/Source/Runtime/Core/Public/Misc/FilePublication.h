#pragma once
#include "CoreAPI.h"
#include "Misc/FilePath.h"
#include "Hash/XxHash.h"

namespace Durin
{
	// Optimistic identity; callers must serialize commits on their owning thread.
	struct FFilePublicationStamp
	{
		bool Exists = false;
		uintmax_t Size = 0;
		std::filesystem::file_time_type Time{};
		auto operator==(const FFilePublicationStamp&) const -> bool = default;
		CORE_API static auto Inspect(const FFilePath& Path, FFilePublicationStamp& Out) -> bool;
	};
	// No format knowledge. A replacement retains its backup until explicit finalization.
	// Multiple file replacements are not crash-atomic.
	struct FFileReplacement
	{
		FFilePath Destination, Staged, Backup;
		bool bBackedUp = false;
		bool bPublished = false;
		CORE_API auto Publish(std::string& Error) -> bool;
		CORE_API auto Rollback(std::string& Error) -> bool;
		CORE_API auto Finalize(std::string& Error) -> bool;
	};
}
