#pragma once

#include <string>

namespace Durin
{
	enum class ETextureBuildOperationFailure
	{
		Failed,
		InvalidInput,
		Canceled
	};

	// Engine has recorded internal failures. Only actionable input text crosses
	// the operation boundary; provider details belong to Engine diagnostics.
	struct FTextureBuildOperationError
	{
		ETextureBuildOperationFailure Code = ETextureBuildOperationFailure::Failed;
		std::string InputReason;
	};

	inline auto FormatTextureBuildOperationError(const FTextureBuildOperationError& Error) -> std::string
	{
		if (Error.Code == ETextureBuildOperationFailure::Canceled) return "Texture build was cancelled.";
		return Error.Code == ETextureBuildOperationFailure::InvalidInput && !Error.InputReason.empty()
			? Error.InputReason : "Texture build failed.";
	}
}
