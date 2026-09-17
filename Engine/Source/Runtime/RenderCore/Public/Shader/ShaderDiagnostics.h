#pragma once

#include "RenderCoreAPI.h"
#include "RHIDefinitions.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	enum class EFeatureInvokeStatus : uint8;

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
		ProviderInvocationFailed,
		ProviderWorkFailed,
		CookInputIdentityUnsupported,
		InvalidProviderCapture,
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
		CompileServiceUnavailable,
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
		FileFingerprintFailure,

		InventoryEmpty,
		Cancelled,
		ProviderUnavailable,
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
		ProviderRequired,
		ProviderForbidden,
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
		std::optional<EFeatureInvokeStatus> ProviderStatus;
		ESlangShaderError CompilerPhase = ESlangShaderError::Session;
		std::optional<int64> NativeStatus;
		std::error_code SystemError;
		std::optional<FFileHelper::FFileIoError> FileError;
		// Reserved for the compiler/provider boundary; never classify this text.
		std::string ExternalDiagnostic;

		auto IsSuccess() const -> bool { return Code == EShaderError::None; }
		RENDERCORE_API static auto FromSlang(
			ESlangShaderError Phase,
			std::string_view Diagnostic,
			std::optional<int64> NativeStatus = {}) -> FShaderError;
		RENDERCORE_API static auto FromFileFingerprint(std::string_view Path, std::string_view Diagnostic) -> FShaderError;
	};

	struct [[nodiscard]] FShaderOperationResult
	{
		FShaderError Error;
		static auto Failure(EShaderError Code) -> FShaderOperationResult
		{
			return {.Error = {.Code = Code}};
		}
		static auto FileSystemFailure(const std::filesystem::path& Path, std::error_code NativeError) -> FShaderOperationResult
		{
			return {.Error = {.Code = EShaderError::FileSystemFailure,
				.ActualIdentity = Path.generic_string(), .SystemError = NativeError}};
		}
		auto IsSuccess() const -> bool { return Error.Code == EShaderError::None; }
		explicit operator bool() const { return IsSuccess(); }
	};

	RENDERCORE_API auto FormatShaderError(const FShaderError& Error) -> std::string;
}
