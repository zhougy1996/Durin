#include "WindowsProcessCrashPolicy.h"

namespace Durin
{
	auto IsValidWindowsProcessCrashSavedDirectory(const std::filesystem::path& SavedDirectory) -> bool
	{
		if (SavedDirectory.empty() || !SavedDirectory.is_absolute()) return false;
		for (const std::filesystem::path& Component : SavedDirectory)
		{
			if (Component == "..") return false;
		}
		return true;
	}

	auto WindowsAccessViolationOperationName(uint64 Operation) -> const char*
	{
		switch (Operation)
		{
		case 0: return "Read";
		case 1: return "Write";
		case 8: return "Execute";
		default: return "Unavailable";
		}
	}

	auto ApplyWindowsProcessCrashRetention(
		const std::filesystem::path& CrashRoot,
		uint32 MaximumCompleteCount,
		uint32 CompleteMaximumAgeDays,
		uint32 PartialMaximumAgeDays) -> FWindowsProcessCrashRetentionResult
	{
		FWindowsProcessCrashRetentionResult Result;
		std::error_code Error;
		if (!std::filesystem::is_directory(CrashRoot, Error)) return Result;
		struct FCandidate
		{
			std::filesystem::path Path;
			std::filesystem::file_time_type Time;
		};
		std::vector<FCandidate> Complete;
		const auto Now = std::filesystem::file_time_type::clock::now();
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(CrashRoot, Error))
		{
			if (Error) break;
			if (Entry.is_symlink(Error) || !Entry.is_directory(Error)) continue;
			const auto WriteTime = Entry.last_write_time(Error);
			if (Error) { Error.clear(); continue; }
			const bool bComplete = std::filesystem::is_regular_file(Entry.path() / "Complete.marker", Error);
			Error.clear();
			const auto Age = Now - WriteTime;
			if (!bComplete)
			{
				if (Age > std::chrono::hours(24 * PartialMaximumAgeDays)
					&& std::filesystem::remove_all(Entry.path(), Error) > 0) ++Result.RemovedPartial;
				Error.clear();
				continue;
			}
			if (Age > std::chrono::hours(24 * CompleteMaximumAgeDays))
			{
				if (std::filesystem::remove_all(Entry.path(), Error) > 0) ++Result.RemovedComplete;
			}
			else Complete.push_back({Entry.path(), WriteTime});
			Error.clear();
		}
		std::ranges::sort(Complete, [](const FCandidate& Left, const FCandidate& Right) { return Left.Time > Right.Time; });
		for (size_t Index = MaximumCompleteCount; Index < Complete.size(); ++Index)
		{
			if (std::filesystem::remove_all(Complete[Index].Path, Error) > 0) ++Result.RemovedComplete;
			Error.clear();
		}
		Result.RetainedComplete = static_cast<uint32>(std::min<size_t>(Complete.size(), MaximumCompleteCount));
		return Result;
	}
}
