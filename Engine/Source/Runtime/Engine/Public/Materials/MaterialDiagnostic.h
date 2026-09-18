#pragma once

#include "EngineAPI.h"
#include "Misc/Guid.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <type_traits>

namespace Durin
{
	struct FMaterialLayoutValidationResult;
	struct FMaterialParameterValidationResult;
	struct FArchiveFailure;
	enum class EMaterialLayoutError : uint8;
	enum class EMaterialProgramValueType : uint8;
	enum class EMaterialParameterError : uint8;
	enum class EArchiveFailureCode : uint8;
	enum class EBulkReadStatus : uint8;
	enum class EPackageResourceReadStatus : uint8;

	enum class EMaterialExpressionError : uint16
	{
		CollectionExceedsAuthoredNodeBound,
		CollectionContainsNullOwnerInvalidGUIDDuplicateGUID,
		ParameterExpressionsRequireValidParameterGUIDsMaterialOwner,
		CollectionExceedsAuthoredInputLinkBound,
		ParameterExpressionMetadataDefaultInvalid,
		CollectionExceedsParameterDeclarationBound,
		ParameterIdentityDistinctExpressionIdentity,
		InputDisconnectedRefersMissingExpression,
		InputsContainCycleExceedTraversalDepthBound,
		BuildReturnedInvalidIRIndex,
		InvalidTextureDefaultConsumer,
		OpcodeResultWidthInputCountInvalid,
		BuildExceedsExpandedIRNodeLinkBound,
		InputIncompatibleType,
		BuildExceedsIRDepthBound,
		NonFiniteConstant,
		SwizzleWidthMismatch,
		SwizzleSelectionExceedsSourceWidth,
		NumericInputRequiresDefaultOneFourComponents,
		ParameterExpressionRequiresValidParameterGUID,
		ParameterExpressionUnsupportedType,
		ParameterGUIDConflictingTypes,
		ParameterCountExceedsBound,
		NumericSignatureMismatch,
		SwizzleSelectOneFourComponents,
		RetainedNumericDefaultInvalidWidthNonFiniteComponent,
		DisconnectedNumericInputOutputSelector,
		SampleExpressionOutputSelectorInvalid,
		RootCountExceedsBound,
		PrimaryOutput,
		DisconnectedAppendInputOutputSelector,
		AppendVectorDefaultContainOneThreeFiniteComponents,
		AppendVectorRequiresNumericInputsTotalingAtMostFourComponents,
		AppendVectorExceedsFourComponents,
		SampleOutputRequiresIndexOutputGUID,
		DisconnectedUVInputOutputSelector,
		SurfaceAttributeOutputSelected,
		SurfaceAttributeInputResolveConstructedSurface,
		SurfaceOverrideInputResolveConstructedSurface,
		SurfaceOverrideContainsInvalidDuplicateAttribute,
		InvalidMaterialOutputCount,
		GraphExceedsNodeLimit,
		IdentityMissingDuplicated,
		ParameterIdentityValidDistinctNodeIdentity,
		SharedParameterDefinitionsDisagree,
		UnsupportedMaterialDomain,
		InvalidMaterialExpressionGraph,
		MultipleOutputNodes,
		InvalidOutputCount,
		MaterialGraphNoOutputNode,
		IdentitiesValidUnique,
		ChildrenSharedBetweenGraphOwners,
		FunctionsOwnRootParametersMaterialOutputs,
		CollectionContainsMissingSharedWronglyOwnedChild,
		OwnerContainsAbandonedExpressionChildOutsideCollection,
		UnableDuplicateExpressionCandidate,
		MaterialOutputSinkUsedAsExpressionSource,
		OutputConnectionsExceedAuthoredLinkBound,
		DisconnectedSurfaceOutputOutputSelector,
		OutputInvalidSelectorRetainedDefault,
		OutputSourceIncompatibleType,
		AggregateMaterialOutputRequiresSurfaceExpression,
		NoTypedExpressionOwner,
		NoAcceptedCompiledProgram,
		BuildOutputNotRegistered,
		BuildOutputAlreadyRegistered,
	};

	enum class EMaterialFunctionError : uint16
	{
		InputTerminalPrimaryOutput,
		OutputTerminalPrimaryOutput,
		CallConnectionsRequireOutputGUIDZeroOutputIndex,
		AuthoringValueInvalidTypeExceedsGraphBounds,
		AuthoringInputInvalid,
		AuthoringValueExceedsExpressionDepth,
		CallPortBindingsExceedBounds,
		CallInputRequiresUniqueValidTypedPort,
		RetainedFunctionBindingDefaultInvalidTypeComponent,
		DisconnectedFunctionBindingOutputSelector,
		BindingTypeMismatch,
		CallOutputRequiresUniqueValidTypedPort,
		CallOutputGUIDBound,
		UnsupportedMaterialExpression,
		InputTerminalNoOwningInvocation,
		InputTerminalNoMatchingDeclaration,
		RequiredFunctionInputNoBinding,
		InputDefaultsContainCycle,
		NumericFunctionDefaultNonNumericType,
		InputNoUsableBindingDefault,
		DefaultTypeMismatch,
		OutputTerminalNoOwningInvocation,
		OutputTerminalNoMatchingDeclaration,
		OutputTypeMismatch,
		CallNoAvailableExpressionBody,
		BuildContainsRecursionExceedsCallDepthBound,
		BuildExceedsDependencyBound,
		ExpressionBodyUnavailable,
		ExpressionBodyExceedsNodeSignatureBounds,
		ExpressionBodyMetadataExceedsClosureByteBound,
		InvalidOutputBinding,
		InvalidInputBinding,
		InvalidTerminalPort,
		OutputNoTerminalExpression,
		CallExceedsPortBounds,
		InputRemovedRetypedBoundMoreThanOnce,
		OutputRemovedRetypedBoundMoreThanOnce,
		RequiredInputUnbound,
		DependencyMissing,
		CallDepthExceedsClosureBound,
		RecursiveCall,
		SharedFunctionDependencyExceedsCallDepthBound,
		DependencyCountPathExceedsClosureBound,
		DistinctFunctionOwnersSameAssetPath,
		NoTypedExpressionBody,
		MetadataExceedsClosureByteBound,
		ChangedDependenciesWereValidated,
		RootCountExceedsAuthoredNodeBound,
		SignatureExceedsInputOutputBounds,
		PortGUIDMissingDuplicated,
		PortTypeNameInvalid,
		OutputsRequiredInputsDeclareDefaults,
		OptionalFunctionInputMissingIncompatibleDefault,
	};

	enum class EMaterialIRError : uint16
	{
		InvalidCompilerEnvironment,
		CompilerDependencyManifestContainsInvalidEntry,
		CompilerDependencyManifestContainsDuplicateVirtualPath,
		SourceMetadataCountExceedsBound,
		SourceMetadataInvalidExceedsBound,
		IdentityUnexpectedlyResolvedZero,
		InvalidStructure,
		OpcodePayloadMismatch,
		AggregateSurfaceRootExpressionInvalid,
		PerPropertySurfaceRootInputInvalid,
		CanonicalBytesExceedVersion2Bound,
		InvalidMaterialIRSlangGeneration,
		AggregateSurfaceRootExpressionOutBounds,
		PerPropertySurfaceRootExpressionOutBounds,
		GeneratedMaterialSlangExceedsVersion1ByteBound,
		ParameterDeclarationCountExceedsBound,
		ParameterDeclarationInvalidDuplicated,
		OpcodeResultTypeInputCountInvalid,
		InputCountExceedsExpandedGraphBound,
		InvalidInputOrder,
		InputTypeMismatch,
		ExpressionDepthExceedsSupportedBound,
		NonFiniteConstant,
		ParameterMissingIncompatibleBindingType,
		SwizzleWidthMismatch,
		SwizzleComponentExceedsInputWidth,
		NonFiniteSurfaceDefault,
		UnsupportedGenerationNode,
	};

	enum class EMaterialPropertyError : uint16
	{
		BlendModeInvalid,
		ShadingModelInvalid,
		DepthWritePolicyInvalid,
		InvalidOpacityMaskThreshold,
		ParentCycleOrDepthExceeded,
		ParentChainNoValidRootMaterial,
		ParentChainContainsUnsupportedMaterialOwner,
	};

	enum class EMaterialCookError : uint16
	{
		CookedProgramIdentityEnvironmentInvalid,
		CookedProgramTargetIncompatible,
		CookedProgramCompilerTargetIdentityIncompatible,
		ActiveParameterCountExceedsLimit,
		ActiveParameterContractInvalid,
		CookedShaderCodeHashInvalid,
		CookedProgramTargetUnsupported,
		CookedProgramExceedsPayloadByteLimit,
		CookedProgramByteExtentInvalid,
		IncompatiblePayloadFormat,
		CookedProgramChecksumInvalid,
		StaticPropertiesMismatch,
		ParameterContractMismatch,
		InvalidArchive,
		ProgramUnavailable,
		PayloadReadFailed,
	};

	enum class EMaterialCompileError : uint16
	{
		CompiledMaterialResultExceedsRetainedResultByteLimit,
		CompilationCanceled,
		AdmissionRejected,
		NoAuthoredProgram,
		ShaderCompilerFailed,
		ShaderEnvironmentUnavailable,
	};

	enum class EMaterialRenderError : uint16
	{
		PayloadSizeMismatch,
		ResourceCountMismatch,
		SamplerCountMismatch,
		SamplingStateFallbackInvalid,
		UniformPayloadContainsNonFiniteValue,
		NonZeroPadding,
		UnsupportedBindingLayout,
		UnsupportedLayoutVersion,
	};

	// Engine-owned errors carry codes and context, never presentation strings.
	struct FMaterialError
	{
		using FCode = std::variant<std::monostate, EMaterialExpressionError,
			EMaterialFunctionError, EMaterialIRError, EMaterialPropertyError,
			EMaterialCookError, EMaterialCompileError, EMaterialRenderError,
			EMaterialLayoutError, EMaterialParameterError>;
		FCode Code;
		std::optional<uint32> Index;
		FGuid ParameterId;
		std::optional<EMaterialProgramValueType> ExpectedType;
		std::optional<EMaterialProgramValueType> ActualType;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		std::optional<EBulkReadStatus> BulkStatus;
		std::optional<EPackageResourceReadStatus> ResourceStatus;
		// Only an external compiler/provider diagnostic may contain opaque text.
		std::string ExternalDiagnostic;

		FMaterialError() = default;
		ENGINE_API FMaterialError(const FMaterialLayoutValidationResult& Validation);
		ENGINE_API FMaterialError(const FMaterialParameterValidationResult& Validation);
		ENGINE_API static auto FromArchive(const FArchiveFailure& Failure) -> FMaterialError;
		template<typename T> requires std::is_constructible_v<FCode, T>
		FMaterialError(T InCode, std::optional<uint32> InIndex = {}) : Code(InCode), Index(InIndex) {}
		template<typename T> requires std::is_constructible_v<FCode, T>
		FMaterialError(T InCode, FGuid InParameterId) : Code(InCode), ParameterId(InParameterId) {}
		ENGINE_API static auto FromExternal(EMaterialCompileError Code, std::string Text) -> FMaterialError;
		ENGINE_API auto HasError() const -> bool;
		auto operator==(const FMaterialError&) const -> bool = default;
	};

	struct FMaterialOperationResult
	{
		FMaterialError Error;
		explicit operator bool() const { return !Error.HasError(); }
	};

	// Presentation boundary: callers choose when to turn semantic errors into text.
	ENGINE_API auto FormatMaterialError(const FMaterialError& Error) -> std::string;
}
