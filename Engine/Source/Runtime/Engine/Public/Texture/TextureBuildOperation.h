#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "DerivedDataStatus.h"
#endif

namespace Durin
{
#if DURIN_WITH_EDITORONLY_DATA
	struct FTextureOperationResult
	{
		DerivedData::EStatus Status = DerivedData::EStatus::Error;
		std::string Diagnostic;
		auto Succeeded() const -> bool { return Status == DerivedData::EStatus::Ok; }
	};
#endif

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
