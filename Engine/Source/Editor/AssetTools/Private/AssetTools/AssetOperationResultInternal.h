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
		if (Result.Effect ==
			EAssetWriteEffect::ContentCommittedProjectionPending)
			State = EAssetOperationTerminalState::ContentCommittedProjectionPending;
		else if (Result.Effect ==
			EAssetWriteEffect::ContentUncertain)
			State = EAssetOperationTerminalState::RecoveryRequired;
		else if (Result.Effect == EAssetWriteEffect::PartiallyWritten)
			State = EAssetOperationTerminalState::PartiallyWritten;
		FAssetOperationResult Operation{
			.Kind = Kind,
			.State = State,
			.Message = Result.Message};
		Operation.AffectedAssets.assign(Affected.begin(), Affected.end());
		return Operation;
	}

}
