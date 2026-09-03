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
		if (Result.Disposition ==
			EAssetResultDisposition::ContentCommittedProjectionPending)
			State = EAssetOperationTerminalState::ContentCommittedProjectionPending;
		else if (Result.Disposition ==
			EAssetResultDisposition::RecoveryRequired)
			State = EAssetOperationTerminalState::RecoveryRequired;
		else if (Result.Disposition ==
			EAssetResultDisposition::ForwardPending)
			State = EAssetOperationTerminalState::ForwardPending;
		FAssetOperationResult Operation{
			.Kind = Kind,
			.State = State,
			.Message = Result.Message,
			.OperationId = Result.OperationId,
			.DesiredDirection = Result.DesiredDirection,
			.FailedParticipant = Result.FailedParticipant,
			.RecoveryLocation = Result.RecoveryLocation};
		Operation.AffectedAssets.assign(Affected.begin(), Affected.end());
		return Operation;
	}

}
