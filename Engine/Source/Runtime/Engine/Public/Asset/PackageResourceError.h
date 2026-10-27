#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"

namespace Durin
{
	// Completed domain outcomes. Request readiness and backend execution stay private.
	enum class EPackageResourceReadStatus : uint8
	{
		Success, InvalidRange, MissingSegment, TruncatedSegment,
		SegmentDigestMismatch, Cancelled, Retired, IoError,
	};
	struct FPackageResourceReadError
	{
		EPackageResourceReadStatus Status = EPackageResourceReadStatus::IoError;
		std::string Message;
	};
	using FPackageResourceReadResult = std::expected<FSharedByteBuffer, FPackageResourceReadError>;
	inline auto GetPackageResourceReadStatus(const FPackageResourceReadResult& Result) -> EPackageResourceReadStatus
	{
		return Result ? EPackageResourceReadStatus::Success : Result.error().Status;
	}
	ENGINE_API auto FormatPackageResourceReadError(const FPackageResourceReadError& Error) -> std::string;
	ENGINE_API auto FormatPackageResourceReadError(const FPackageResourceReadResult& Result) -> std::string;

	enum class EPreparedPackageResourceError : uint8 { InvalidClosure, BudgetExceeded, IoError, Stale, Cancelled };
	struct FPreparedPackageResourceError
	{
		EPreparedPackageResourceError Code = EPreparedPackageResourceError::InvalidClosure;
		std::string Message;
	};
	ENGINE_API auto FormatPreparedPackageResourceError(const FPreparedPackageResourceError& Error) -> std::string;

	enum class EPackageResourceRegistrationError : uint8
	{
		PackageBusy, EmptySegment, InvalidMetadata, ShuttingDown, InvalidGeneration,
	};
	struct FPackageResourceRegistrationError
	{
		EPackageResourceRegistrationError Code = EPackageResourceRegistrationError::InvalidMetadata;
		std::string Message;
	};
	ENGINE_API auto FormatPackageResourceRegistrationError(const FPackageResourceRegistrationError& Error) -> std::string;
}
