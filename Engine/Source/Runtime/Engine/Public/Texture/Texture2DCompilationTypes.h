#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "EngineAPI.h"
#include "Texture/Texture2DBuildTypes.h"
#include "DerivedDataStatus.h"

namespace Durin
{
	// Identifies the externally visible phase of one object-level Texture2D compilation.
	enum class ETexture2DCompilationPhase : uint8
	{
		None,
		Queued,
		Building,
		AwaitingApply,
		Ready,
		Failed,
		Canceled,
	};

	enum class ETexture2DCompilationPriority : uint8
	{
		Background,
		Interactive,
	};

	// Records wall-clock build timing and conservative versus observed retained bytes.
	struct FTexture2DCompilationMetrics
	{
		// Dispatch start to build completion, including asynchronous cache I/O.
		uint64 BuildElapsedNanoseconds = 0;
		uint64 CompletionNanoseconds = 0;
		uint64 EstimatedBytes = 0;
		uint64 ResultBytes = 0;
	};

	enum class ETexture2DCompilationError : uint8
	{
		None, BuildFailed,
		InvalidSource, MissingSourceIdentity, ManagerUnavailable, ManagerNotStarted, InvalidOwner,
		AdmissionRejected, MissingPackage, InvalidProduct, SourceMismatch, InvalidSettings,
	};
	struct FTexture2DCompilationError
	{
		ETexture2DCompilationError Code = ETexture2DCompilationError::None;
		// Synchronous admission/application failure; asynchronous results carry diagnostics directly.
		std::string Diagnostic;
		std::optional<FTexture2DInputError> InputCause;
		std::string ObjectPath;
		FXxHash128 ExpectedSourceIdentity{};
		FXxHash128 ActualSourceIdentity{};
		auto HasError() const -> bool { return Code != ETexture2DCompilationError::None; }
	};
	ENGINE_API auto FormatTexture2DCompilationError(const FTexture2DCompilationError& Error) -> std::string;

}

#endif
