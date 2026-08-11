#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Reports bounded healthy-start cleanup without exposing crash-handler state.
	struct FWindowsProcessCrashRetentionResult
	{
		uint32 RemovedComplete = 0;
		uint32 RemovedPartial = 0;
		uint32 RetainedComplete = 0;
	};

	auto IsValidWindowsProcessCrashSavedDirectory(const std::filesystem::path& SavedDirectory) -> bool;
	auto WindowsAccessViolationOperationName(uint64 Operation) -> const char*;
	auto ApplyWindowsProcessCrashRetention(
		const std::filesystem::path& CrashRoot,
		uint32 MaximumCompleteCount,
		uint32 CompleteMaximumAgeDays,
		uint32 PartialMaximumAgeDays) -> FWindowsProcessCrashRetentionResult;
}
