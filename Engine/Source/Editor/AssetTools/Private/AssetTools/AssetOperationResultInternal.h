#pragma once

#include "AssetTools/AssetOperation.h"
#include "Asset/AssetWriteResult.h"

namespace Durin::AssetToolsPrivate
{
	inline auto FromEngineResult(
		EAssetOperationKind Kind,
		const FAssetWriteResult& Result,
		std::span<const FPackagePath> Affected = {}) -> FAssetOperationResult
	{
		EAssetOperationTerminalState State = Result
			? EAssetOperationTerminalState::Completed
			: EAssetOperationTerminalState::Rejected;
		if (Result.Disposition ==
			EAssetWriteDisposition::ContentCommittedProjectionPending)
			State = EAssetOperationTerminalState::ContentCommittedProjectionPending;
		else if (Result.Disposition ==
			EAssetWriteDisposition::RecoveryRequired)
			State = EAssetOperationTerminalState::RecoveryRequired;
		else if (Result.Disposition ==
			EAssetWriteDisposition::ForwardPending)
			State = EAssetOperationTerminalState::ForwardPending;
		else if (Result.Disposition == EAssetWriteDisposition::PartiallyWritten)
			State = EAssetOperationTerminalState::PartiallyWritten;
		FAssetOperationResult Operation{
			.Kind = Kind,
			.State = State,
			.Message = Result.Message,
			.FailedParticipant = Result.FailedParticipant,
			.RecoveryLocation = Result.RecoveryLocation,
			.AffectedFiles = Result.AffectedFiles};
		Operation.AffectedAssets.assign(Affected.begin(), Affected.end());
		return Operation;
	}

}
