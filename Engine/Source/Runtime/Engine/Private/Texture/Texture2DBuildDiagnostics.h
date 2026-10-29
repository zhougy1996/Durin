#pragma once
#include "CoreMinimal.h"
#include "Texture/Texture2DBuild.h"

#if DURIN_WITH_EDITORONLY_DATA
namespace Durin
{
	// Engine orchestration diagnostics; recipes report failures through module logs.
	enum class ETexture2DBuildError : uint8
	{
		InvalidInput,
		MissingSourceIdentity, AuthoredBuildUnavailable, InvalidBuilderVersion, Cancelled,
		InvalidBuilderProduct, ModuleUnavailable,
	};
	struct FTexture2DBuildError
	{
		ETexture2DBuildError Code = ETexture2DBuildError::InvalidInput;
		std::optional<FTexture2DInputError> InputCause;
		std::string Description; // Already formatted at the generic build boundary.
	};
	ENGINE_API auto FormatTexture2DBuildError(const FTexture2DBuildError& Error) -> std::string;

	enum class ETexture2DCompilationError : uint8
	{
		BuildFailed,
		InvalidSource, MissingSourceIdentity, ManagerUnavailable, ManagerNotStarted, InvalidOwner,
		AdmissionRejected, MissingPackage, InvalidProduct, SourceMismatch, InvalidSettings,
	};
	struct FTexture2DCompilationError
	{
		ETexture2DCompilationError Code = ETexture2DCompilationError::BuildFailed;
		// Synchronous admission/application failure; asynchronous results carry diagnostics directly.
		std::string Diagnostic;
		std::optional<FTexture2DInputError> InputCause;
		std::string ObjectPath;
		FXxHash128 ExpectedSourceIdentity{};
		FXxHash128 ActualSourceIdentity{};
	};
	ENGINE_API auto FormatTexture2DCompilationError(const FTexture2DCompilationError& Error) -> std::string;

	ENGINE_API auto ReportTextureCompilationFailure(const FTexture2DCompilationError& Error) -> FTextureBuildOperationError;

	// This cancellation value is borrowed only for the duration of Build.
	struct FTexture2DBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
	};

	// Returns a complete value-owned CPU product; failures expose no partial product.
	ENGINE_API auto BuildTexture2DPlatformData(const FTexture2DBuildRequest& Request,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr)
		-> std::expected<FTexturePlatformData, FTexture2DBuildError>;
}
#endif
