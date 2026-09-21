#pragma once

#include "EngineAPI.h"

namespace Durin
{
	enum class EAssetWriteError : uint8
	{
		None, InvalidPath, AlreadyExists, NotFound, IoError, InvalidData,
		UnsupportedVersion, InUse, StaleData, ReadOnlyMode, ShuttingDown, Cancelled,
		ProjectionPending
	};
	enum class EAssetWriteEffect : uint8
	{
		None, ContentCommittedProjectionPending, PartiallyWritten, ContentUncertain
	};
	// Describes observed write effects, never whether an owning job can retry.
	struct FAssetWriteResult
	{
		EAssetWriteError Error = EAssetWriteError::None;
		std::string Message;
		EAssetWriteEffect Effect = EAssetWriteEffect::None;
		std::string FailedParticipant;
		// Retained backup/staging data for manual repair, never a replay locator.
		std::filesystem::path RecoveryLocation;
		std::vector<std::filesystem::path> AffectedFiles;
		auto Succeeded() const -> bool { return Error == EAssetWriteError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
}
