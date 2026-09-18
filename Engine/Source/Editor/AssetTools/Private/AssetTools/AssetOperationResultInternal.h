#pragma once

#include "AssetTools/AssetOperation.h"
#include "Asset/AssetDefinitions.h"

namespace Durin::AssetToolsPrivate
{
	inline auto FromEngineResult(
		EAssetOperationKind Kind,
		const FAssetResult& Result,
		std::span<const FPackagePath> Affected = {}) -> FAssetOperationResult
	{
		EAssetOperationTerminalState State = Result
			? EAssetOperationTerminalState::Completed
			: EAssetOperationTerminalState::Rejected;
		if (Result.WriteOutcome.Disposition ==
			EAssetResultDisposition::ContentCommittedProjectionPending)
			State = EAssetOperationTerminalState::ContentCommittedProjectionPending;
		else if (Result.WriteOutcome.Disposition ==
			EAssetResultDisposition::RecoveryRequired)
			State = EAssetOperationTerminalState::RecoveryRequired;
		else if (Result.WriteOutcome.Disposition ==
			EAssetResultDisposition::ForwardPending)
			State = EAssetOperationTerminalState::ForwardPending;
		FAssetOperationResult Operation{
			.Kind = Kind,
			.State = State,
			.Message = Result.Message,
			.OperationId = Result.WriteOutcome.OperationId,
			.DesiredDirection = Result.WriteOutcome.DesiredDirection,
			.FailedParticipant = Result.WriteOutcome.FailedParticipant,
			.RecoveryLocation = Result.WriteOutcome.RecoveryLocation};
		Operation.AffectedAssets.assign(Affected.begin(), Affected.end());
		return Operation;
	}

}
