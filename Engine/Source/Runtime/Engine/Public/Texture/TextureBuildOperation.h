#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"

namespace Durin
{
#if DURIN_WITH_EDITORONLY_DATA
	// Object application policy for explicit source builds. Caching the current
	// asset preserves its source and package state without caller-supplied policy.
	struct FTextureBuildApplicationContext
	{
		bool bMarkPackageDirty = true;
	};

	enum class ETextureCompilationStatus : uint8 { Succeeded, Failed, Canceled };
	struct FTextureOperationResult
	{
		ETextureCompilationStatus Status = ETextureCompilationStatus::Failed;
		std::string Diagnostic;
		auto Succeeded() const -> bool { return Status == ETextureCompilationStatus::Succeeded; }
	};
	using FTextureCompilationCompletion = std::function<void(FTextureOperationResult)>;
#endif

	enum class ETextureBuildOperationFailure
	{
		Failed,
		InvalidInput,
		Unavailable,
		Busy,
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
		if (Error.Code == ETextureBuildOperationFailure::Unavailable) return "Texture build services are unavailable.";
		if (Error.Code == ETextureBuildOperationFailure::Busy) return "Texture build services are busy; retry later.";
		if (Error.Code == ETextureBuildOperationFailure::Canceled) return "Texture build was cancelled.";
		return Error.Code == ETextureBuildOperationFailure::InvalidInput && !Error.InputReason.empty()
			? Error.InputReason : "Texture build failed.";
	}
}
