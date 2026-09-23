#pragma once

#include "RenderCoreAPI.h"
#include "RHIDefinitions.h"
#include "Misc/FileError.h"
#include <expected>

namespace Durin
{
	enum class ESlangShaderError : uint8
	{
		Session, Layout, Code, Module, EntryPoint, Dependencies
	};

	enum class EShaderCaptureLimit : uint8
	{
		Mounts, DirectoryEntries, Files, PathBytes, FileBytes, TotalBytes
	};

	struct FShaderCaptureLimitContext
	{
		EShaderCaptureLimit Kind;
		uint64 Maximum;
		uint64 Actual;
	};

	enum class EShaderError : uint8
	{
		None,
		CookInputIdentityUnsupported,
		CaptureMountLimit,
		FileSystemFailure,
		CaptureDirectoryLimit,
		CaptureSymlink,
		FileReadFailure,
		CaptureInputLimit,
		CaptureDuplicateFile,
		DependencyNotCaptured,
		DependencyIdentityMissing,
		InvalidCompileRequest,
		InvalidGeneratedRequest,
		ImportNotDeclared,
		ShaderMountRequired,
		ImportNotAllowed,
		DependencyResolutionFailed,
		DependencyContentConflict,
		CaptureFileLimit,
		CaptureInputInvalid,
		CaptureSearchRootLimit,
		CaptureSearchRootInvalid,
		ReflectionDescriptorSetInvalid,
		ReflectionBindingTypeUnsupported,
		ReflectionRegisterInvalid,
		ReflectionPushConstantSizeUnsupported,
		ReflectionUnavailable,
		SpirvConversionFailed,
		MissingEntryPoints,
		CompilationNotStarted,
		SlangFailure,

		InventoryEmpty,
		Cancelled,
		BuildModuleUnavailable,
		RequestRetirementFrozen,
		RequestNullBuildType,
		RequestRegistrationFrozen,
		RequestAlreadyRegistered,
		RequestBuildTypeCountMismatch,
		RequestBuildTypesMismatch,

		RequestHeaderInvalid,
		RequestMembersInvalid,
		RequestBuildTypesUnavailable,
		InventoryTargetInvalid,
		InventoryTargetFrozen,
		InventoryNameAmbiguous,
		InventoryIdentityDuplicate,
		LibraryRecordCountInvalid,
		LibraryRecordInvalid,
		LibraryIdentityDuplicate,
		LibraryDirectoryOverflow,
		LibraryPayloadOffsetOverflow,
		LibraryTooLarge,
		LibraryReadFailed,
		LibraryExtentInvalid,
		LibraryHeaderInvalid,
		LibraryDirectoryRecordInvalid,
		LibraryDirectoryOrderInvalid,
		LibraryInventoryDigestInvalid,
		LibraryRequiredRequestMissing,
		LibraryNotOpen,
		LibraryRequestUnavailable,
		LibraryPayloadDigestInvalid,
		BuildModuleRequired,
		BuildModuleForbidden,
		DataConfigurationInvalid,
		DataAlreadyInitialized,
		CookedDomainRequired,
		RequestUnregistered,
		RequestTypeCountMismatch,
		RequestTypesMismatch,

		PayloadRequestInvalid,
		PayloadOutputInvalid,
		PayloadBindingInvalid,
		PayloadPushConstantInvalid,
		PayloadTooLarge,
		PayloadInputInvalid,
		PayloadHeaderInvalid,
		PayloadEntryInvalid,
		PayloadSpirvInvalid,
		PayloadSpirvHashMismatch,
		PayloadBindingCountInvalid,
		PayloadPushConstantCountInvalid,
		PayloadTrailingBytes,

		DuplicateMacro,
		MissingVirtualPath,
		VirtualPathMismatch,
		ShaderTypePathMismatch,
		EntryPointMismatch,
		FrequencyMismatch,
		EntryPointCountMismatch,
		FrequencyCountMismatch,
		EmptyParameterName,
		MissingParameter,
		ParameterTypeMismatch,
		ParameterArraySizeMismatch,
		BindingTypeConflict,
		BindingArraySizeConflict,
		PushConstantOverlap,
		CompiledShaderCountMismatch,
		CompiledFrequencyMismatch,
		CompiledEntryPointMismatch,
		ShaderInstanceCreationFailed,
		EmptyShaderSet,
		InvalidOpacityMaskThreshold,
		MaterialProgramInvalid,
		MaterialLayoutMismatch,
		MaterialPassContractMismatch,
		MaterialProgramIdentityMismatch,
		MaterialTargetMismatch,
		NullShaderType,
		MissingVertexFactory,
		MissingMaterialShaderType,
		MissingShaderType,
		RHIShaderCreationFailed,
		MissingGeneratedStage,
		MissingResourceCode,

	};

	// Engine-owned failures retain identities and numeric context, never prose.
	struct FShaderError
	{
		EShaderError Code = EShaderError::None;
		std::string ShaderType;
		std::string Parameter;
		std::string ExpectedIdentity;
		std::string ActualIdentity;
		uint64 Index = 0;
		std::optional<uint64> ElementIndex;
		uint64 Expected = 0;
		uint64 Actual = 0;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		uint32 ExistingBegin = 0;
		uint32 ExistingEnd = 0;
		uint32 NewBegin = 0;
		uint32 NewEnd = 0;
		std::optional<FShaderCaptureLimitContext> CaptureLimit;
		ESlangShaderError CompilerPhase = ESlangShaderError::Session;
		std::optional<int64> NativeStatus;
		std::optional<FFileError> FileError;
		// Reserved for the compiler boundary; never classify this text.
		std::string ExternalDiagnostic;

		// In-process diagnostic identity; excludes external wording, not a persistent cache key.
		RENDERCORE_API auto GetSemanticFingerprint() const -> size_t;
		auto IsSuccess() const -> bool { return Code == EShaderError::None; }
		static auto FromFileSystem(const std::filesystem::path& Path, std::error_code Error) -> FShaderError
		{
			return {.Code = EShaderError::FileSystemFailure,
				.FileError = FFileError{EFileOperation::Inspect, Error, Path}};
		}
		RENDERCORE_API static auto FromSlang(
			ESlangShaderError Phase,
			std::string_view Diagnostic,
			std::optional<int64> NativeStatus = {}) -> FShaderError;
	};

	// Completed operations carry either success or one structured failure.
	using FShaderOperationResult = std::expected<void, FShaderError>;


	RENDERCORE_API auto FormatShaderError(const FShaderError& Error) -> std::string;
}
