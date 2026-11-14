#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "EngineAPI.h"
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
		std::variant<std::string, UnifiedError::FError, std::shared_ptr<const FCookInputFailure>> Cause;
		auto GetError() const -> const UnifiedError::FError* { return std::get_if<UnifiedError::FError>(&Cause); }
		auto GetDiagnostic() const -> std::shared_ptr<const FCookInputFailure>
		{
			const auto* Failure = std::get_if<std::shared_ptr<const FCookInputFailure>>(&Cause);
			return Failure ? *Failure : nullptr;
		}
		auto ToString() const -> std::string
		{
			if (const auto* Text = std::get_if<std::string>(&Cause)) return *Text;
			if (const auto* Error = GetError()) return Error->ToString();
			const auto Failure = GetDiagnostic();
			return Failure ? FormatCookInputError(*Failure) : std::string{};
		}
		auto Succeeded() const -> bool { return Status == ECookInputStatus::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	inline auto ToCookInputResult(const std::expected<void, Durin::UnifiedError::FError>& Result) -> FCookInputResult
	{
		if (Result) return {};
		const auto& Error = Result.error();
		const auto Status = Error.Is(AssetLoadErrors::Cancelled) ? ECookInputStatus::Cancelled
			: Error.Is(PackageReadErrors::IoError) || Error.Is({"AssetRegistry", "IoError"}) ? ECookInputStatus::IoError
			: Error.Is(AssetLoadErrors::ProjectionPending) ? ECookInputStatus::ProjectionPending
			: ECookInputStatus::InvalidDependency;
		return {Status, Error};
	}
}
