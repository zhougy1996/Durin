#pragma once
#include "Asset/AssetReadResult.h"
namespace Durin
{
	enum class ECookInputStatus : uint8
	{
		None, ProjectionPending, UndeclaredInput, InvalidDependency, LimitExceeded, IoError, Cancelled
	};
	struct FCookInputResult
	{
		ECookInputStatus Status = ECookInputStatus::None;
		std::string Message;
		auto Succeeded() const -> bool { return Status == ECookInputStatus::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	inline auto ToCookInputResult(const FAssetReadResult& Result) -> FCookInputResult
	{
		if (Result) return {};
		const auto Status = Result.Error == EAssetReadError::Cancelled ? ECookInputStatus::Cancelled
		 : Result.Error == EAssetReadError::IoError ? ECookInputStatus::IoError
		 : Result.Error == EAssetReadError::ProjectionPending ? ECookInputStatus::ProjectionPending
		 : ECookInputStatus::InvalidDependency;
		return {Status, Result.Message};
	}
}
