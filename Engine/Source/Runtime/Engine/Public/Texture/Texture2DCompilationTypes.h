#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "EngineAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	// Identifies the externally visible phase of one object-level Texture2D compilation.
	enum class ETexture2DCompilationPhase : uint8
	{
		None,
		Queued,
		Preparing,
		Building,
		UploadPending,
		Ready,
		Failed,
		Cancelled,
	};

	enum class ETexture2DCompilationPriority : uint8
	{
		Background,
		Interactive,
	};

	// Records worker timing and conservative versus observed retained bytes.
	struct FTexture2DCompilationMetrics
	{
		uint64 PreparationNanoseconds = 0;
		uint64 WorkerNanoseconds = 0;
		uint64 CompletionNanoseconds = 0;
		uint64 EstimatedBytes = 0;
		uint64 ResultBytes = 0;
	};

	enum class ETexture2DCompilationError : uint8
	{
		None, BuildFailed, WorkerFailed, Cancelled, CancelledBeforeAdmission, CancelledDuringShutdown,
		InvalidSource, MissingSourceIdentity, ManagerUnavailable, ManagerNotStarted, InvalidOwner,
		AdmissionRejected, MissingPackage, InvalidProduct, SourceMismatch, InvalidSettings,
		InputMismatch, Superseded, ImportValidation, ImportAllocation, Save,
	};
	struct FTexture2DBuildInputIdentity;
	struct FAssetImportDataError;
	struct FAssetWriteResult;
	struct FTexture2DCompilationError
	{
		ETexture2DCompilationError Code = ETexture2DCompilationError::None;
		// Concise actionable input reason; internal build causes stay in diagnostics.
		std::string InputReason;
		std::optional<ETaskState> TaskState;
		std::optional<FTexture2DInputError> InputCause;
		std::string ObjectPath;
		FXxHash128 ExpectedSourceIdentity{};
		FXxHash128 ActualSourceIdentity{};
		std::shared_ptr<const FTexture2DBuildInputIdentity> ExpectedInput;
		std::shared_ptr<const FTexture2DBuildInputIdentity> ActualInput;
		std::shared_ptr<const FAssetImportDataError> ImportCause;
		std::shared_ptr<const FAssetWriteResult> SaveCause;
		auto HasError() const -> bool { return Code != ETexture2DCompilationError::None; }
	};
	ENGINE_API auto FormatTexture2DCompilationError(const FTexture2DCompilationError& Error) -> std::string;

}

#endif
