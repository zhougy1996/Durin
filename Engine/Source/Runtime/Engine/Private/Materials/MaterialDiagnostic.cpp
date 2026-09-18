#include "Materials/MaterialDiagnostic.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialProgramTypes.h"
#include "Serialization/Archive.h"

namespace Durin
{

	FMaterialError::FMaterialError(const FMaterialLayoutValidationResult& Validation)
		: Code(Validation.Error), Index(Validation.FieldIndex), ParameterId(Validation.ParameterId) {}

	FMaterialError::FMaterialError(const FMaterialParameterValidationResult& Validation)
		: Code(Validation.Error), ParameterId(Validation.ParameterId) {}

	auto FMaterialError::FromArchive(const FArchiveFailure& Failure) -> FMaterialError
	{
		FMaterialError Error(EMaterialCookError::InvalidArchive);
		Error.ArchiveCode = Failure.Code;
		Error.ArchivePath = Failure.Path.substr(0, MaterialProgramMaxDiagnosticMessageBytes);
		return Error;
	}

	auto FMaterialError::HasError() const -> bool
	{
		return std::visit([](auto Value) {
			using T = decltype(Value);
			if constexpr (std::is_same_v<T, std::monostate>) return false;
			else if constexpr (std::is_same_v<T, EMaterialLayoutError>) return Value != EMaterialLayoutError::None;
			else if constexpr (std::is_same_v<T, EMaterialParameterError>) return Value != EMaterialParameterError::None;
			else return true;
		}, Code);
	}

	auto FMaterialError::FromExternal(EMaterialCompileError Code, std::string Text) -> FMaterialError
	{
		FMaterialError Error(Code);
		Text.resize(std::min<size_t>(Text.size(), MaterialProgramMaxDiagnosticMessageBytes));
		Error.ExternalDiagnostic = std::move(Text);
		return Error;
	}

	auto FormatMaterialError(const FMaterialError& Error) -> std::string
	{
		auto Text = std::visit([](auto Code) -> std::string {
			using T = decltype(Code);
			if constexpr (std::is_same_v<T, std::monostate>) return {};
			else if constexpr (std::is_same_v<T, EMaterialLayoutError>) return std::string(GetMaterialLayoutErrorText(Code));
			else if constexpr (std::is_same_v<T, EMaterialParameterError>) return std::string(GetMaterialParameterErrorText(Code));
			else if constexpr (std::is_same_v<T, EMaterialExpressionError>)
			{
				switch (Code)
				{
				case EMaterialExpressionError::CollectionExceedsAuthoredNodeBound: return "Expression collection exceeds the authored node bound.";
				case EMaterialExpressionError::CollectionContainsNullOwnerInvalidGUIDDuplicateGUID: return "Expression collection contains a null owner, invalid GUID, or duplicate GUID.";
				case EMaterialExpressionError::ParameterExpressionsRequireValidParameterGUIDsMaterialOwner: return "Parameter expressions require valid parameter GUIDs and a material owner.";
				case EMaterialExpressionError::CollectionExceedsAuthoredInputLinkBound: return "Expression collection exceeds the authored input/link bound.";
				case EMaterialExpressionError::ParameterExpressionMetadataDefaultInvalid: return "Parameter expression metadata or default is invalid.";
				case EMaterialExpressionError::CollectionExceedsParameterDeclarationBound: return "Expression collection exceeds the parameter declaration bound.";
				case EMaterialExpressionError::ParameterIdentityDistinctExpressionIdentity: return "Parameter identity must be distinct from expression identity.";
				case EMaterialExpressionError::InputDisconnectedRefersMissingExpression: return "Expression input is disconnected or refers to a missing expression.";
				case EMaterialExpressionError::InputsContainCycleExceedTraversalDepthBound: return "Expression inputs contain a cycle or exceed the traversal depth bound.";
				case EMaterialExpressionError::BuildOutputNotRegistered: return "Expression Build did not register the requested output.";
				case EMaterialExpressionError::BuildOutputAlreadyRegistered: return "Expression Build registered an output more than once.";
				case EMaterialExpressionError::BuildReturnedInvalidIRIndex: return "Expression Build registered an invalid IR index.";
				case EMaterialExpressionError::InvalidTextureDefaultConsumer: return "A texture default may only be consumed by sampling or a function texture port.";
				case EMaterialExpressionError::OpcodeResultWidthInputCountInvalid: return "Expression opcode, result width, or input count is invalid.";
				case EMaterialExpressionError::BuildExceedsExpandedIRNodeLinkBound: return "Expression Build exceeds the expanded IR node or link bound.";
				case EMaterialExpressionError::InputIncompatibleType: return "Expression input has an incompatible type.";
				case EMaterialExpressionError::BuildExceedsIRDepthBound: return "Expression Build exceeds the IR depth bound.";
				case EMaterialExpressionError::NonFiniteConstant: return "Expression constant must be finite.";
				case EMaterialExpressionError::SwizzleWidthMismatch: return "Swizzle selection does not match its result width.";
				case EMaterialExpressionError::SwizzleSelectionExceedsSourceWidth: return "Swizzle selection exceeds the source width.";
				case EMaterialExpressionError::NumericInputRequiresDefaultOneFourComponents: return "Numeric input requires a default of one to four components.";
				case EMaterialExpressionError::ParameterExpressionRequiresValidParameterGUID: return "Parameter expression requires a valid parameter GUID.";
				case EMaterialExpressionError::ParameterExpressionUnsupportedType: return "Parameter expression has an unsupported type.";
				case EMaterialExpressionError::ParameterGUIDConflictingTypes: return "Parameter GUID has conflicting types.";
				case EMaterialExpressionError::ParameterCountExceedsBound: return "Expression parameter count exceeds the bound.";
				case EMaterialExpressionError::NumericSignatureMismatch: return "Numeric expression signature does not match its inputs.";
				case EMaterialExpressionError::SwizzleSelectOneFourComponents: return "Swizzle must select one to four components.";
				case EMaterialExpressionError::RetainedNumericDefaultInvalidWidthNonFiniteComponent: return "Retained numeric default has an invalid width or non-finite component.";
				case EMaterialExpressionError::DisconnectedNumericInputOutputSelector: return "Disconnected numeric input has an output selector.";
				case EMaterialExpressionError::SampleExpressionOutputSelectorInvalid: return "Sample expression output selector is invalid.";
				case EMaterialExpressionError::RootCountExceedsBound: return "Expression root count exceeds the bound.";
				case EMaterialExpressionError::PrimaryOutput: return "Expression has only its primary output.";
				case EMaterialExpressionError::DisconnectedAppendInputOutputSelector: return "Disconnected append input has an output selector.";
				case EMaterialExpressionError::AppendVectorDefaultContainOneThreeFiniteComponents: return "Append Vector default must contain one to three finite components.";
				case EMaterialExpressionError::AppendVectorRequiresNumericInputsTotalingAtMostFourComponents: return "Append Vector requires numeric inputs totaling at most four components.";
				case EMaterialExpressionError::AppendVectorExceedsFourComponents: return "Append Vector exceeds four components.";
				case EMaterialExpressionError::SampleOutputRequiresIndexOutputGUID: return "Sample output requires an index, not an output GUID.";
				case EMaterialExpressionError::DisconnectedUVInputOutputSelector: return "Disconnected UV input has an output selector.";
				case EMaterialExpressionError::SurfaceAttributeOutputSelected: return "Surface attribute output is not selected.";
				case EMaterialExpressionError::SurfaceAttributeInputResolveConstructedSurface: return "Surface attribute input must resolve to a constructed Surface.";
				case EMaterialExpressionError::SurfaceOverrideInputResolveConstructedSurface: return "Surface override input must resolve to a constructed Surface.";
				case EMaterialExpressionError::SurfaceOverrideContainsInvalidDuplicateAttribute: return "Surface override contains an invalid or duplicate attribute.";
				case EMaterialExpressionError::InvalidMaterialOutputCount: return "A material graph requires exactly one material output node.";
				case EMaterialExpressionError::GraphExceedsNodeLimit: return "Material graph exceeds the node limit.";
				case EMaterialExpressionError::IdentityMissingDuplicated: return "Material expression identity is missing or duplicated.";
				case EMaterialExpressionError::ParameterIdentityValidDistinctNodeIdentity: return "Parameter identity must be valid and distinct from node identity.";
				case EMaterialExpressionError::SharedParameterDefinitionsDisagree: return "Shared parameter definitions disagree.";
				case EMaterialExpressionError::UnsupportedMaterialDomain: return "Unsupported material domain.";
				case EMaterialExpressionError::InvalidMaterialExpressionGraph: return "Invalid material expression graph.";
				case EMaterialExpressionError::MultipleOutputNodes: return "A material graph cannot contain multiple output nodes.";
				case EMaterialExpressionError::InvalidOutputCount: return "A material graph requires exactly one output node.";
				case EMaterialExpressionError::MaterialGraphNoOutputNode: return "The material graph has no output node.";
				case EMaterialExpressionError::IdentitiesValidUnique: return "Expression identities must be valid and unique.";
				case EMaterialExpressionError::ChildrenSharedBetweenGraphOwners: return "Expression children cannot be shared between graph owners.";
				case EMaterialExpressionError::FunctionsOwnRootParametersMaterialOutputs: return "Functions cannot own root parameters or material outputs.";
				case EMaterialExpressionError::CollectionContainsMissingSharedWronglyOwnedChild: return "Expression collection contains a missing, shared, or wrongly owned child.";
				case EMaterialExpressionError::OwnerContainsAbandonedExpressionChildOutsideCollection: return "Owner contains an abandoned expression child outside its collection.";
				case EMaterialExpressionError::UnableDuplicateExpressionCandidate: return "Unable to duplicate the expression candidate.";
				case EMaterialExpressionError::MaterialOutputSinkUsedAsExpressionSource: return "A material output is a sink and cannot be used as an expression source.";
				case EMaterialExpressionError::OutputConnectionsExceedAuthoredLinkBound: return "Material output connections exceed the authored link bound.";
				case EMaterialExpressionError::DisconnectedSurfaceOutputOutputSelector: return "Disconnected Surface output has an output selector.";
				case EMaterialExpressionError::OutputInvalidSelectorRetainedDefault: return "Material output has an invalid selector or retained default.";
				case EMaterialExpressionError::OutputSourceIncompatibleType: return "Material output source has an incompatible type.";
				case EMaterialExpressionError::AggregateMaterialOutputRequiresSurfaceExpression: return "Aggregate material output requires a Surface expression.";
				case EMaterialExpressionError::NoTypedExpressionOwner: return "Material has no typed expression owner.";
				case EMaterialExpressionError::NoAcceptedCompiledProgram: return "Material has no accepted compiled program.";
				}
			}
			else if constexpr (std::is_same_v<T, EMaterialFunctionError>)
			{
				switch (Code)
				{
				case EMaterialFunctionError::InputTerminalPrimaryOutput: return "Function input terminal has only its primary output.";
				case EMaterialFunctionError::OutputTerminalPrimaryOutput: return "Function output terminal has only its primary output.";
				case EMaterialFunctionError::CallConnectionsRequireOutputGUIDZeroOutputIndex: return "Function call connections require an output GUID and zero output index.";
				case EMaterialFunctionError::AuthoringValueInvalidTypeExceedsGraphBounds: return "Function authoring value has an invalid type or exceeds graph bounds.";
				case EMaterialFunctionError::AuthoringInputInvalid: return "Function authoring input is invalid.";
				case EMaterialFunctionError::AuthoringValueExceedsExpressionDepth: return "Function authoring value exceeds expression depth.";
				case EMaterialFunctionError::CallPortBindingsExceedBounds: return "Function call port bindings exceed their bounds.";
				case EMaterialFunctionError::CallInputRequiresUniqueValidTypedPort: return "Function call input requires a unique valid typed port.";
				case EMaterialFunctionError::RetainedFunctionBindingDefaultInvalidTypeComponent: return "Retained function binding default has an invalid type or component.";
				case EMaterialFunctionError::DisconnectedFunctionBindingOutputSelector: return "Disconnected function binding has an output selector.";
				case EMaterialFunctionError::BindingTypeMismatch: return "Function binding value does not match its declared type.";
				case EMaterialFunctionError::CallOutputRequiresUniqueValidTypedPort: return "Function call output requires a unique valid typed port.";
				case EMaterialFunctionError::CallOutputGUIDBound: return "Function call output GUID is not bound.";
				case EMaterialFunctionError::UnsupportedMaterialExpression: return "Functions cannot declare material parameters or material outputs.";
				case EMaterialFunctionError::InputTerminalNoOwningInvocation: return "Function input terminal has no owning invocation.";
				case EMaterialFunctionError::InputTerminalNoMatchingDeclaration: return "Function input terminal has no matching declaration.";
				case EMaterialFunctionError::RequiredFunctionInputNoBinding: return "Required function input has no binding.";
				case EMaterialFunctionError::InputDefaultsContainCycle: return "Function input defaults contain a cycle.";
				case EMaterialFunctionError::NumericFunctionDefaultNonNumericType: return "Numeric function default has a non-numeric type.";
				case EMaterialFunctionError::InputNoUsableBindingDefault: return "Function input has no usable binding or default.";
				case EMaterialFunctionError::DefaultTypeMismatch: return "Function default type does not match its input declaration.";
				case EMaterialFunctionError::OutputTerminalNoOwningInvocation: return "Function output terminal has no owning invocation.";
				case EMaterialFunctionError::OutputTerminalNoMatchingDeclaration: return "Function output terminal has no matching declaration.";
				case EMaterialFunctionError::OutputTypeMismatch: return "Function output source does not match its declared type.";
				case EMaterialFunctionError::CallNoAvailableExpressionBody: return "Function call has no available expression body.";
				case EMaterialFunctionError::BuildContainsRecursionExceedsCallDepthBound: return "Function Build contains recursion or exceeds the call depth bound.";
				case EMaterialFunctionError::BuildExceedsDependencyBound: return "Function Build exceeds the dependency bound.";
				case EMaterialFunctionError::ExpressionBodyUnavailable: return "Function expression body is unavailable.";
				case EMaterialFunctionError::ExpressionBodyExceedsNodeSignatureBounds: return "Function expression body exceeds node or signature bounds.";
				case EMaterialFunctionError::ExpressionBodyMetadataExceedsClosureByteBound: return "Function expression body metadata exceeds the closure byte bound.";
				case EMaterialFunctionError::InvalidOutputBinding: return "Function call output binding does not match a unique typed port.";
				case EMaterialFunctionError::InvalidInputBinding: return "Function call input binding does not match a unique typed port.";
				case EMaterialFunctionError::InvalidTerminalPort: return "Function terminal does not match a unique declared port.";
				case EMaterialFunctionError::OutputNoTerminalExpression: return "Function output has no terminal expression.";
				case EMaterialFunctionError::CallExceedsPortBounds: return "Function call exceeds port bounds.";
				case EMaterialFunctionError::InputRemovedRetypedBoundMoreThanOnce: return "Function input was removed, retyped or bound more than once.";
				case EMaterialFunctionError::OutputRemovedRetypedBoundMoreThanOnce: return "Function output was removed, retyped or bound more than once.";
				case EMaterialFunctionError::RequiredInputUnbound: return "Required function input is not connected or bound.";
				case EMaterialFunctionError::DependencyMissing: return "Function dependency is missing.";
				case EMaterialFunctionError::CallDepthExceedsClosureBound: return "Function call depth exceeds the closure bound.";
				case EMaterialFunctionError::RecursiveCall: return "Recursive function calls are not supported.";
				case EMaterialFunctionError::SharedFunctionDependencyExceedsCallDepthBound: return "Shared function dependency exceeds the call depth bound.";
				case EMaterialFunctionError::DependencyCountPathExceedsClosureBound: return "Function dependency count or path exceeds the closure bound.";
				case EMaterialFunctionError::DistinctFunctionOwnersSameAssetPath: return "Distinct function owners have the same asset path.";
				case EMaterialFunctionError::NoTypedExpressionBody: return "Function has no typed expression body.";
				case EMaterialFunctionError::MetadataExceedsClosureByteBound: return "Function metadata exceeds the closure byte bound.";
				case EMaterialFunctionError::ChangedDependenciesWereValidated: return "Function changed while its dependencies were validated.";
				case EMaterialFunctionError::RootCountExceedsAuthoredNodeBound: return "Function root count exceeds the authored node bound.";
				case EMaterialFunctionError::SignatureExceedsInputOutputBounds: return "Function signature exceeds input/output bounds.";
				case EMaterialFunctionError::PortGUIDMissingDuplicated: return "Function port GUID is missing or duplicated.";
				case EMaterialFunctionError::PortTypeNameInvalid: return "Function port type or name is invalid.";
				case EMaterialFunctionError::OutputsRequiredInputsDeclareDefaults: return "Outputs and required inputs cannot declare defaults.";
				case EMaterialFunctionError::OptionalFunctionInputMissingIncompatibleDefault: return "Optional function input has a missing or incompatible default.";
				}
			}
			else if constexpr (std::is_same_v<T, EMaterialIRError>)
			{
				switch (Code)
				{
				case EMaterialIRError::InvalidCompilerEnvironment: return "Material compiler environment identity, target, pass contract, or dependency bounds are invalid.";
				case EMaterialIRError::CompilerDependencyManifestContainsInvalidEntry: return "Material compiler dependency manifest contains an invalid entry.";
				case EMaterialIRError::CompilerDependencyManifestContainsDuplicateVirtualPath: return "Material compiler dependency manifest contains a duplicate virtual path.";
				case EMaterialIRError::SourceMetadataCountExceedsBound: return "IR source metadata count exceeds its bound.";
				case EMaterialIRError::SourceMetadataInvalidExceedsBound: return "IR source metadata is invalid or exceeds its bound.";
				case EMaterialIRError::IdentityUnexpectedlyResolvedZero: return "Material IR identity unexpectedly resolved to zero.";
				case EMaterialIRError::InvalidStructure: return "Material IR version, node count, or surface output count is invalid.";
				case EMaterialIRError::OpcodePayloadMismatch: return "Material IR opcode and immediate payload do not agree.";
				case EMaterialIRError::AggregateSurfaceRootExpressionInvalid: return "Material IR aggregate Surface Root expression is invalid.";
				case EMaterialIRError::PerPropertySurfaceRootInputInvalid: return "Material IR per-property Surface Root input is invalid.";
				case EMaterialIRError::CanonicalBytesExceedVersion2Bound: return "Material IR canonical bytes exceed the version-2 bound.";
				case EMaterialIRError::InvalidMaterialIRSlangGeneration: return "Invalid material IR for Slang generation.";
				case EMaterialIRError::AggregateSurfaceRootExpressionOutBounds: return "Aggregate Surface Root expression is out of bounds.";
				case EMaterialIRError::PerPropertySurfaceRootExpressionOutBounds: return "Per-property Surface Root expression is out of bounds.";
				case EMaterialIRError::GeneratedMaterialSlangExceedsVersion1ByteBound: return "Generated material Slang exceeds the version-1 byte bound.";
				case EMaterialIRError::ParameterDeclarationCountExceedsBound: return "IR parameter declaration count exceeds its bound.";
				case EMaterialIRError::ParameterDeclarationInvalidDuplicated: return "IR parameter declaration is invalid or duplicated.";
				case EMaterialIRError::OpcodeResultTypeInputCountInvalid: return "Material IR opcode, result type, or input count is invalid.";
				case EMaterialIRError::InputCountExceedsExpandedGraphBound: return "Material IR input count exceeds the expanded graph bound.";
				case EMaterialIRError::InvalidInputOrder: return "Material IR inputs must refer to an earlier expression.";
				case EMaterialIRError::InputTypeMismatch: return "Material IR input type does not match its opcode signature.";
				case EMaterialIRError::ExpressionDepthExceedsSupportedBound: return "Material IR expression depth exceeds the supported bound.";
				case EMaterialIRError::NonFiniteConstant: return "Material IR constant components must be finite.";
				case EMaterialIRError::ParameterMissingIncompatibleBindingType: return "Material IR parameter is missing or has an incompatible binding type.";
				case EMaterialIRError::SwizzleWidthMismatch: return "Material IR swizzle length does not match its result width.";
				case EMaterialIRError::SwizzleComponentExceedsInputWidth: return "Material IR swizzle component exceeds its input width.";
				case EMaterialIRError::NonFiniteSurfaceDefault: return "Material IR surface defaults must be finite.";
				case EMaterialIRError::UnsupportedGenerationNode: return "Material IR node cannot be generated.";
				}
			}
			else if constexpr (std::is_same_v<T, EMaterialPropertyError>)
			{
				switch (Code)
				{
				case EMaterialPropertyError::BlendModeInvalid: return "Material blend mode is invalid.";
				case EMaterialPropertyError::ShadingModelInvalid: return "Material shading model is invalid.";
				case EMaterialPropertyError::DepthWritePolicyInvalid: return "Material depth-write policy is invalid.";
				case EMaterialPropertyError::InvalidOpacityMaskThreshold: return "Material opacity-mask threshold must be finite and in the inclusive range [0, 1].";
				case EMaterialPropertyError::ParentCycleOrDepthExceeded: return "Material parent chain exceeds 64 owners or contains a cycle.";
				case EMaterialPropertyError::ParentChainNoValidRootMaterial: return "Material parent chain has no valid root material.";
				case EMaterialPropertyError::ParentChainContainsUnsupportedMaterialOwner: return "Material parent chain contains an unsupported material owner.";
				}
			}
			else if constexpr (std::is_same_v<T, EMaterialCookError>)
			{
				switch (Code)
				{
				case EMaterialCookError::CookedProgramIdentityEnvironmentInvalid: return "Material cooked program identity or environment is invalid.";
				case EMaterialCookError::CookedProgramTargetIncompatible: return "Material cooked program target is incompatible.";
				case EMaterialCookError::CookedProgramCompilerTargetIdentityIncompatible: return "Material cooked program compiler or target identity is incompatible.";
				case EMaterialCookError::ActiveParameterCountExceedsLimit: return "Material active parameter count exceeds its limit.";
				case EMaterialCookError::ActiveParameterContractInvalid: return "Material active parameter contract is invalid.";
				case EMaterialCookError::CookedShaderCodeHashInvalid: return "Material cooked shader code hash is invalid.";
				case EMaterialCookError::CookedProgramTargetUnsupported: return "Material cooked program target is unsupported.";
				case EMaterialCookError::CookedProgramExceedsPayloadByteLimit: return "Material cooked program exceeds its payload byte limit.";
				case EMaterialCookError::CookedProgramByteExtentInvalid: return "Material cooked program byte extent is invalid.";
				case EMaterialCookError::IncompatiblePayloadFormat: return "Material cooked program format is incompatible; recook from authored assets.";
				case EMaterialCookError::CookedProgramChecksumInvalid: return "Material cooked program checksum is invalid.";
				case EMaterialCookError::StaticPropertiesMismatch: return "payload static properties do not match package metadata.";
				case EMaterialCookError::ParameterContractMismatch: return "payload parameter contract does not match package metadata.";
				case EMaterialCookError::InvalidArchive: return "Material cooked archive is invalid.";
				case EMaterialCookError::ProgramUnavailable: return "Material cooked program is unavailable.";
				case EMaterialCookError::PayloadReadFailed: return "Material cooked payload could not be read.";
				}
			}
			else if constexpr (std::is_same_v<T, EMaterialCompileError>)
			{
				switch (Code)
				{
				case EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit: return "Compiled material result exceeds the retained-result byte limit.";
				case EMaterialCompileError::CompilationCanceled: return "Material compilation was canceled.";
				case EMaterialCompileError::AdmissionRejected: return "Material compile admission was rejected.";
				case EMaterialCompileError::NoAuthoredProgram: return "Material has no authored program.";
				case EMaterialCompileError::ShaderCompilerFailed: return "Material compiler provider failed.";
				case EMaterialCompileError::ShaderEnvironmentUnavailable: return "Material compiler environment is unavailable.";
				}
			}
			else if constexpr (std::is_same_v<T, EMaterialRenderError>)
			{
				switch (Code)
				{
				case EMaterialRenderError::PayloadSizeMismatch: return "Material render uniform payload size does not match its layout.";
				case EMaterialRenderError::ResourceCountMismatch: return "Material render resource count does not match its layout.";
				case EMaterialRenderError::SamplerCountMismatch: return "Material sampler and fallback counts must match texture resources.";
				case EMaterialRenderError::SamplingStateFallbackInvalid: return "Material sampling state or fallback is invalid.";
				case EMaterialRenderError::UniformPayloadContainsNonFiniteValue: return "Material render uniform payload contains a non-finite value.";
				case EMaterialRenderError::NonZeroPadding: return "Material render uniform padding must be zero.";
				case EMaterialRenderError::UnsupportedBindingLayout: return "Only compiled material layout v4 can bind.";
				case EMaterialRenderError::UnsupportedLayoutVersion: return "Material render layouts require compiled version 4; migrate and recompile older assets.";
				}
			}

			return "Unknown material error.";
		}, Error.Code);
		if (Error.ParameterId.IsValid()) Text += std::format(" [parameter {}]", Error.ParameterId.ToString());
		if (Error.ExpectedType) Text += std::format(" [expected type {}]", static_cast<uint8>(*Error.ExpectedType));
		if (Error.ActualType) Text += std::format(" [actual type {}]", static_cast<uint8>(*Error.ActualType));
		if (Error.Index) Text += std::format(" [index {}]", *Error.Index);
		if (!Error.ArchivePath.empty()) Text += std::format(" [archive path {}]", Error.ArchivePath);
		if (Error.ArchiveCode) Text += std::format(" [archive code {}]", static_cast<uint8>(*Error.ArchiveCode));
		if (Error.BulkStatus) Text += std::format(" [bulk status {}]", static_cast<uint8>(*Error.BulkStatus));
		if (Error.ResourceStatus) Text += std::format(" [resource status {}]", static_cast<uint8>(*Error.ResourceStatus));
		if (!Error.ExternalDiagnostic.empty()) Text += " " + Error.ExternalDiagnostic;
		return Text;
	}

	auto GetMaterialParameterErrorText(EMaterialParameterError Error) -> std::string_view
	{
		switch (Error)
		{
		case EMaterialParameterError::None: return "";
		case EMaterialParameterError::TooManyDefinitions: return "Material declaration count exceeds its bound.";
		case EMaterialParameterError::InvalidId: return "Material declaration GUID is invalid.";
		case EMaterialParameterError::DuplicateId: return "Material declaration GUID is duplicated.";
		case EMaterialParameterError::InvalidName: return "Material declaration name must not be None.";
		case EMaterialParameterError::DuplicateName: return "Material declaration name is already occupied.";
		case EMaterialParameterError::InvalidText: return "Material declaration text is invalid or exceeds its bound.";
		case EMaterialParameterError::InvalidType: return "Material declaration type is invalid.";
		case EMaterialParameterError::InvalidDefault: return "Material declaration default must be finite.";
		case EMaterialParameterError::InvalidMetadata: return "Material declaration metadata is invalid.";
		case EMaterialParameterError::NotFound: return "Material declaration does not exist.";
		case EMaterialParameterError::TypeConflict: return "Changing a declaration type requires a new GUID and an unoccupied name.";
		case EMaterialParameterError::UnsupportedProgramSchema: return "Unsupported material program schema.";
		case EMaterialParameterError::InvalidProgram: return "Material declaration change would produce an invalid program.";
		}
		return "Unknown material parameter error.";
	}


	auto GetMaterialLayoutErrorText(EMaterialLayoutError Error) -> std::string_view
	{
		switch (Error)
		{
		case EMaterialLayoutError::None: return {};
		case EMaterialLayoutError::InvalidParameter: return "The material layout contains an invalid parameter identity.";
		case EMaterialLayoutError::DuplicateParameter: return "The material layout contains a duplicate parameter identity.";
		case EMaterialLayoutError::InvalidType: return "The material layout contains an unsupported parameter type.";
		case EMaterialLayoutError::ResourceLimit: return "The material layout exceeds the target uniform or descriptor budget.";
		case EMaterialLayoutError::InvalidField: return "The material layout fields, counts or offsets do not match its deterministic schema.";
		case EMaterialLayoutError::InvalidIdentity: return "The material layout version or identity does not match its schema.";
		case EMaterialLayoutError::InvalidReflection: return "The compiled material reflection does not match the layout and pass contract.";
		}
		return "Unknown material layout error.";
	}

}
