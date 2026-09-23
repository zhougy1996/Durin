#include "Shader/ShaderDiagnostics.h"

namespace Durin
{
	auto FShaderError::GetSemanticFingerprint() const -> size_t
	{
		size_t Fingerprint = 0;
		auto Add = [&]<typename T>(const T& Value) {
			Fingerprint ^= std::hash<T>{}(Value) + 0x9e3779b9 + (Fingerprint << 6) + (Fingerprint >> 2);
		};
		auto AddNative = [&](const std::error_code& Error) {
			Add(Error.value());
			Add(std::string(Error.category().name()));
		};
		Add(Code); Add(ShaderType); Add(Parameter);
		Add(ExpectedIdentity); Add(ActualIdentity);
		Add(Index); Add(ElementIndex); Add(Expected); Add(Actual);
		Add(SetIndex); Add(BindingIndex);
		Add(ExistingBegin); Add(ExistingEnd); Add(NewBegin); Add(NewEnd);
		Add(CompilerPhase); Add(NativeStatus);
		Add(CaptureLimit.has_value());
		if (CaptureLimit)
		{
			Add(CaptureLimit->Kind); Add(CaptureLimit->Maximum); Add(CaptureLimit->Actual);
		}
		Add(FileError.has_value());
		if (FileError)
		{
			Add(FileError->Operation); AddNative(FileError->NativeError);
			Add(FileError->Path.generic_string()); Add(FileError->RelatedPath.generic_string());
			Add(FileError->Range.has_value());
			if (FileError->Range) { Add(FileError->Range->Offset); Add(FileError->Range->Size); }
		}
		return Fingerprint;
	}


	namespace
	{
		auto CaptureLimitName(EShaderCaptureLimit Kind) -> std::string_view
		{
			switch (Kind)
			{
			case EShaderCaptureLimit::Mounts: return "mount count";
			case EShaderCaptureLimit::DirectoryEntries: return "directory entry count";
			case EShaderCaptureLimit::Files: return "file count";
			case EShaderCaptureLimit::PathBytes: return "path bytes";
			case EShaderCaptureLimit::FileBytes: return "file bytes";
			case EShaderCaptureLimit::TotalBytes: return "total bytes";
			}
			return "unknown limit";
		}

		auto ShaderCompilerPhaseName(ESlangShaderError Phase) -> std::string_view
		{
			switch (Phase)
			{
			case ESlangShaderError::Session: return "session creation";
			case ESlangShaderError::Layout: return "layout reflection";
			case ESlangShaderError::Code: return "code generation";
			case ESlangShaderError::Module: return "module loading";
			case ESlangShaderError::EntryPoint: return "entry-point compilation";
			case ESlangShaderError::Dependencies: return "dependency resolution";
			}
			return "unknown operation";
		}
	}

	auto FShaderError::FromSlang(
		ESlangShaderError Phase,
		std::string_view Diagnostic,
		std::optional<int64> Status) -> FShaderError
	{
		return {.Code = EShaderError::SlangFailure, .CompilerPhase = Phase, .NativeStatus = Status,
			.ExternalDiagnostic = std::string(Diagnostic.substr(0, 4096))};
	}

	auto FormatShaderError(const FShaderError& Error) -> std::string
	{
		if (!Error.IsSuccess() && Error.CaptureLimit)
		{
			const auto& Limit = *Error.CaptureLimit;
			return std::format("Shader capture {} limit exceeded for '{}': maximum {}, actual {}.",
				CaptureLimitName(Limit.Kind), Error.ActualIdentity, Limit.Maximum, Limit.Actual);
		}
		switch (Error.Code)
		{
		case EShaderError::None: return {};
		case EShaderError::CookInputIdentityUnsupported: return "ShaderBuild module does not declare Cook input identity.";
		case EShaderError::CaptureMountLimit: return "Shader mount limit exceeded.";
		case EShaderError::FileSystemFailure:
			return Error.FileError ? Error.FileError->ToString() : "Shader filesystem operation failed.";
		case EShaderError::CaptureDirectoryLimit: return "Shader directory entry limit exceeded.";
		case EShaderError::CaptureSymlink: return std::format("Shader capture requires regular source files, not symbolic links: '{}'.", Error.ActualIdentity);
		case EShaderError::FileReadFailure:
			return Error.FileError ? Error.FileError->ToString() : "Shader file read failed.";
		case EShaderError::CaptureInputLimit: return "Shader capture input limit exceeded.";
		case EShaderError::CaptureDuplicateFile: return std::format("Shader capture contains duplicate logical file '{}'.", Error.ActualIdentity);
		case EShaderError::DependencyNotCaptured: return std::format("Shader dependency '{}' was not captured.", Error.ActualIdentity);
		case EShaderError::DependencyIdentityMissing:
			return std::format("Shader dependency '{}' has no registered virtual identity.", Error.ActualIdentity);
		case EShaderError::InvalidCompileRequest: return "Shader entry-point request is invalid.";
		case EShaderError::InvalidGeneratedRequest: return "Generated shader request is invalid.";
		case EShaderError::ImportNotDeclared: return std::format("Generated shader import '{}' is not declared.", Error.ActualIdentity);
		case EShaderError::ShaderMountRequired: return "Generated shader compilation requires a registered shader mount";
		case EShaderError::ImportNotAllowed: return std::format("Generated shader import '{}' is not allowlisted.", Error.ActualIdentity);
		case EShaderError::DependencyResolutionFailed: return "Failed to parse shader dependency graph";
		case EShaderError::DependencyContentConflict: return std::format("Shader virtual dependency '{}' resolves to conflicting content.", Error.ActualIdentity);
		case EShaderError::CaptureFileLimit: return "Captured shader file count exceeds the limit.";
		case EShaderError::CaptureInputInvalid: return "Captured shader paths or bytes exceed canonical input limits.";
		case EShaderError::CaptureSearchRootLimit: return "Captured shader search root limit exceeded.";
		case EShaderError::CaptureSearchRootInvalid: return "Invalid captured shader search root.";
		case EShaderError::ReflectionDescriptorSetInvalid:
			return std::format("Shader parameter '{}' has an invalid descriptor set.", Error.Parameter);
		case EShaderError::ReflectionBindingTypeUnsupported:
			return std::format("Shader parameter '{}' has unsupported binding type {}.", Error.Parameter, Error.Actual);
		case EShaderError::ReflectionRegisterInvalid:
			return std::format("Shader parameter '{}' has an invalid register.", Error.Parameter);
		case EShaderError::ReflectionPushConstantSizeUnsupported:
			return std::format("Shader parameter '{}' has an unsupported push constant size.", Error.Parameter);
		case EShaderError::ReflectionUnavailable: return "Failed to access Slang program layout reflection";
		case EShaderError::SpirvConversionFailed: return "Failed to convert Slang SPIR-V output";
		case EShaderError::MissingEntryPoints: return "No entry points found";
		case EShaderError::CompilationNotStarted: return "Shader compilation has not completed.";
		case EShaderError::SlangFailure:
			return std::format("Slang phase {} failed (status {}): {}", ShaderCompilerPhaseName(Error.CompilerPhase), Error.NativeStatus ? std::to_string(*Error.NativeStatus) : "unavailable", Error.ExternalDiagnostic);

		case EShaderError::InventoryEmpty: return "Cooked Shader inventory is empty.";
		case EShaderError::Cancelled: return "Shader operation cancelled.";
		case EShaderError::BuildModuleUnavailable: return "ShaderBuild module is unavailable.";
		case EShaderError::RequestRetirementFrozen: return "Shader request retirement is forbidden after inventory freeze.";
		case EShaderError::RequestNullBuildType: return "Shader request contains a null build type.";
		case EShaderError::RequestRegistrationFrozen: return "Shader request registration is forbidden after inventory freeze.";
		case EShaderError::RequestAlreadyRegistered: return "Shader request owner/name is already registered.";
		case EShaderError::RequestBuildTypeCountMismatch: return "Shader request build-type count does not match its members.";
		case EShaderError::RequestBuildTypesMismatch: return "Shader request build types do not match canonical members.";

		case EShaderError::RequestHeaderInvalid: return "Shader runtime request header is invalid.";
		case EShaderError::RequestMembersInvalid: return "Shader runtime request members are invalid or non-canonical.";
		case EShaderError::RequestBuildTypesUnavailable: return "Shader request has no registered build-type contribution.";
		case EShaderError::InventoryTargetInvalid: return "Shader inventory target is invalid.";
		case EShaderError::InventoryTargetFrozen: return "Shader inventory is already frozen for another target.";
		case EShaderError::InventoryNameAmbiguous: return "Shader inventory contains an ambiguous request name.";
		case EShaderError::InventoryIdentityDuplicate: return "Shader inventory contains a duplicate identity.";
		case EShaderError::LibraryRecordCountInvalid: return "Shader library target or record count is invalid.";
		case EShaderError::LibraryRecordInvalid: return "Shader library record target or production identity is invalid.";
		case EShaderError::LibraryIdentityDuplicate: return "Shader library contains a duplicate runtime identity.";
		case EShaderError::LibraryDirectoryOverflow: return "Shader library directory extent overflowed.";
		case EShaderError::LibraryPayloadOffsetOverflow: return "Shader library payload offset overflowed.";
		case EShaderError::LibraryTooLarge: return "Shader library exceeds its byte bound.";
		case EShaderError::LibraryReadFailed: return Error.FileError ? Error.FileError->ToString() : "Shader library could not be read.";
		case EShaderError::LibraryExtentInvalid: return "Shader library byte extent is invalid.";
		case EShaderError::LibraryHeaderInvalid: return "Shader library header is incompatible or corrupt.";
		case EShaderError::LibraryDirectoryRecordInvalid: return "Shader library directory record is invalid.";
		case EShaderError::LibraryDirectoryOrderInvalid: return "Shader library directory is unsorted or duplicated.";
		case EShaderError::LibraryInventoryDigestInvalid: return "Shader library extent or inventory digest is invalid.";
		case EShaderError::LibraryRequiredRequestMissing: return "Shader library is missing a required runtime request.";
		case EShaderError::LibraryNotOpen: return "Shader library is not open.";
		case EShaderError::LibraryRequestUnavailable: return "Shader library request is unavailable.";
		case EShaderError::LibraryPayloadDigestInvalid: return "Shader library payload digest is invalid.";
		case EShaderError::BuildModuleRequired: return "Authored Shader data requires a ShaderBuild module.";
		case EShaderError::BuildModuleForbidden: return "Cooked Shader data forbids a ShaderBuild module.";
		case EShaderError::DataConfigurationInvalid: return "Cooked Shader data configuration is invalid.";
		case EShaderError::DataAlreadyInitialized: return "Shader data domain is already initialized.";
		case EShaderError::CookedDomainRequired: return "Cooked Shader data was requested outside the Cooked domain.";
		case EShaderError::RequestUnregistered: return std::format("Cooked Shader request '{}' is not registered.", Error.ActualIdentity);
		case EShaderError::RequestTypeCountMismatch: return "Cooked Shader request type count does not match registration.";
		case EShaderError::RequestTypesMismatch: return "Cooked Shader request types do not match registration.";

		case EShaderError::PayloadRequestInvalid: return "Shader payload request or output count is invalid.";
		case EShaderError::PayloadOutputInvalid: return "Shader payload contains invalid compiled output.";
		case EShaderError::PayloadBindingInvalid: return "Shader payload resource reflection is invalid.";
		case EShaderError::PayloadPushConstantInvalid: return "Shader payload push constants are invalid.";
		case EShaderError::PayloadTooLarge: return "Shader payload exceeds its DDC value limit.";
		case EShaderError::PayloadInputInvalid: return "Shader payload request or byte count is invalid.";
		case EShaderError::PayloadHeaderInvalid: return "Shader payload header is incompatible or malformed.";
		case EShaderError::PayloadEntryInvalid: return "Shader payload entry identity is invalid.";
		case EShaderError::PayloadSpirvInvalid: return "Shader payload SPIR-V is invalid.";
		case EShaderError::PayloadSpirvHashMismatch: return "Shader payload SPIR-V hash is invalid.";
		case EShaderError::PayloadBindingCountInvalid: return "Shader payload binding count is invalid.";
		case EShaderError::PayloadPushConstantCountInvalid: return "Shader payload push-constant count is invalid.";
		case EShaderError::PayloadTrailingBytes: return "Shader payload contains trailing bytes.";

		case EShaderError::EmptyShaderSet:
			return "Material shader map requires at least one shader type.";
		case EShaderError::InvalidOpacityMaskThreshold:
			return "Material shader identity has a non-finite opacity mask threshold.";
		case EShaderError::MaterialProgramInvalid: return "Accepted material compiler result is invalid.";
		case EShaderError::MaterialLayoutMismatch:
			return std::format("Material layout '{}' version {} does not match '{}' version {}.",
				Error.ActualIdentity, Error.Actual, Error.ExpectedIdentity, Error.Expected);
		case EShaderError::MaterialPassContractMismatch:
			return std::format("Material pass contract {} does not match {}.", Error.Actual, Error.Expected);
		case EShaderError::MaterialProgramIdentityMismatch:
			return std::format(
				"Compiled material program identity '{}' does not match requested identity '{}'.",
				Error.ActualIdentity, Error.ExpectedIdentity);
		case EShaderError::MaterialTargetMismatch:
			return std::format(
				"Compiled material target '{}' does not match requested target '{}'.",
				Error.ActualIdentity, Error.ExpectedIdentity);
		case EShaderError::NullShaderType:
			return "Material shader map contains a null shader type.";
		case EShaderError::MissingVertexFactory:
			return std::format("Mesh Material shader type '{}' requires a Vertex Factory type.", Error.ShaderType);
		case EShaderError::MissingMaterialShaderType:
			return "Material shader map contains no registered Material shader type.";
		case EShaderError::MissingShaderType:
			return std::format("Material shader map is missing type '{}'.", Error.ShaderType);
		case EShaderError::RHIShaderCreationFailed:
			return std::format("RHI shader creation returned null for Material type '{}'.", Error.ShaderType);
		case EShaderError::MissingGeneratedStage:
			return std::format(
				"Accepted material program has no {} stage for type '{}'.",
				Error.ExpectedIdentity, Error.ShaderType);
		case EShaderError::MissingResourceCode:
			return std::format("Fixed Material shader type '{}' produced no resource code.", Error.ShaderType);

		case EShaderError::DuplicateMacro:
			return std::format("Duplicate shader macro definition is not allowed: {}", Error.Parameter);
		case EShaderError::MissingVirtualPath:
			return "Shader type virtual shader path must not be empty";
		case EShaderError::VirtualPathMismatch:
			return std::format(
				"Shader compile options virtual path '{}' does not match shader type virtual path '{}'",
				Error.ActualIdentity, Error.ExpectedIdentity);
		case EShaderError::ShaderTypePathMismatch:
			return std::format(
				"Shader type '{}' uses path '{}' but shader map expects '{}'",
				Error.ShaderType, Error.ActualIdentity, Error.ExpectedIdentity);
		case EShaderError::EntryPointMismatch:
			return std::format(
				"Shader compile options entry point at index {} does not match shader type '{}'",
				Error.Index, Error.ShaderType);
		case EShaderError::FrequencyMismatch:
			return std::format(
				"Shader compile options frequency at index {} does not match shader type '{}'",
				Error.Index, Error.ShaderType);
		case EShaderError::EntryPointCountMismatch:
			return std::format(
				"Shader compile options entry point count ({}) does not match shader type count ({})",
				Error.Actual, Error.Expected);
		case EShaderError::FrequencyCountMismatch:
			return std::format(
				"Shader compile options frequency count ({}) does not match shader type count ({})",
				Error.Actual, Error.Expected);
		case EShaderError::EmptyParameterName:
			return "Shader parameter metadata contains an empty name";
		case EShaderError::MissingParameter:
			return std::format("Shader parameter '{}' was not found in shader reflection", Error.Parameter);
		case EShaderError::ParameterTypeMismatch:
			return std::format(
				"Shader parameter '{}' type does not match reflection (expected {}, actual {})",
				Error.Parameter, Error.Expected, Error.Actual);
		case EShaderError::ParameterArraySizeMismatch:
			return std::format(
				"Shader parameter '{}' array size does not match reflection (expected {}, actual {})",
				Error.Parameter, Error.Expected, Error.Actual);
		case EShaderError::BindingTypeConflict:
			return std::format(
				"Conflicting shader binding types at set {}, binding {} (expected {}, actual {})",
				Error.SetIndex, Error.BindingIndex, Error.Expected, Error.Actual);
		case EShaderError::BindingArraySizeConflict:
			return std::format(
				"Conflicting shader binding array sizes at set {}, binding {} (expected {}, actual {})",
				Error.SetIndex, Error.BindingIndex, Error.Expected, Error.Actual);
		case EShaderError::PushConstantOverlap:
			return std::format(
				"Conflicting push constant ranges: existing [{}..{}), new [{}..{})",
				Error.ExistingBegin, Error.ExistingEnd, Error.NewBegin, Error.NewEnd);
		case EShaderError::CompiledShaderCountMismatch:
			return std::format(
				"Shader type count ({}) does not match compiled shader count ({})",
				Error.Expected, Error.Actual);
		case EShaderError::CompiledFrequencyMismatch:
			return std::format(
				"Compiled shader frequency does not match shader type '{}' at index {} (expected {}, actual {})",
				Error.ShaderType, Error.Index, Error.Expected, Error.Actual);
		case EShaderError::CompiledEntryPointMismatch:
			return std::format(
				"Compiled shader entry point '{}' does not match shader type '{}' entry point '{}'",
				Error.ActualIdentity, Error.ShaderType, Error.ExpectedIdentity);
		case EShaderError::ShaderInstanceCreationFailed:
			return std::format("Shader type '{}' failed to create a shader instance", Error.ShaderType);
		}
		return "Unknown shader error";
	}
}
