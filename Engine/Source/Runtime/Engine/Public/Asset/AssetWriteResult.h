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
	enum class EAssetWriteDisposition : uint8
	{
		Default, ForwardPending, ContentCommittedProjectionPending,
		RecoveryRequired, PartiallyWritten
	};
	// Only write operations can report durable progress and recovery metadata.
	struct FAssetWriteResult
	{
		EAssetWriteError Error = EAssetWriteError::None;
		std::string Message;
		EAssetWriteDisposition Disposition = EAssetWriteDisposition::Default;
		std::string FailedParticipant;
		// Retained backup/staging data for manual repair, never a replay locator.
		std::filesystem::path RecoveryLocation;
		std::vector<std::filesystem::path> AffectedFiles;
		auto Succeeded() const -> bool { return Error == EAssetWriteError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
}
