#pragma once
#include "Asset/AssetReadResult.h"
namespace Durin
{
	struct FCookInputFailure;
	ENGINE_API auto FormatCookInputError(const FCookInputFailure& Failure) -> std::string;
	enum class ECookInputStatus : uint8
	{
		None, ProjectionPending, UndeclaredInput, InvalidDependency, LimitExceeded, IoError, Cancelled
	};
	struct FCookInputResult
	{
		ECookInputStatus Status = ECookInputStatus::None;
		// Legacy boundaries supply text; discovery retains its structured cause
		// and formats only when a presentation boundary requests it.
		std::variant<std::string, std::shared_ptr<const FCookInputFailure>> Cause;
		auto GetDiagnostic() const -> std::shared_ptr<const FCookInputFailure>
		{
			const auto* Failure = std::get_if<std::shared_ptr<const FCookInputFailure>>(&Cause);
			return Failure ? *Failure : nullptr;
		}
		auto ToString() const -> std::string
		{
			if (const auto* Text = std::get_if<std::string>(&Cause)) return *Text;
			const auto Failure = GetDiagnostic();
			return Failure ? FormatCookInputError(*Failure) : std::string{};
		}
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
