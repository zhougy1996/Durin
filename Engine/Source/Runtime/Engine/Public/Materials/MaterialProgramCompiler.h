#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialCompilationConfiguration.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialParameterValues.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Materials/MaterialCustomOutput.h"
#include "Materials/MaterialParameterCollection.h"
#include "Shader/MaterialShaderIdentity.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/Shader.h"
#include "Materials/MaterialBindingSchema.h"

namespace Durin
{
	class DMaterialInterface;

	namespace MIR
	{
		inline constexpr uint32 CurrentVersion = 16;
	}
	// Opaque producer identity; algorithm changes invalidate builds, not DMAT admission.
	ENGINE_API auto GetMaterialBuildVersion() -> uint64;
	ENGINE_API auto GetMaterialCompileAdapterVersion() -> uint32;
	inline constexpr uint32 CurrentMaterialPassContractVersion = 30;
	inline constexpr uint32 MaterialProgramMaxShaderEntries = 64;
	// Classification only. Persisted requests use Shader type/entry identities.
	enum class EMaterialShaderEntry : uint8 { Forward, GBuffer, Shadow, HitProxy, LocalVertex, SplineVertex, PostProcess, SingleLayerWater, SingleLayerWaterDepth, SkyBackground };
	using FMaterialShaderEntrySet = std::vector<std::string>;
	struct FMaterialShaderEntryDescriptor
	{
		EMaterialShaderEntry Entry;
		std::string_view ShaderType;
		std::string_view Name;
		EShaderFrequency Frequency;
		uint8 GeometryUsage;
		uint8 PassUsage;
		bool bEditorOnly = false;
		EMaterialDomain Domain = EMaterialDomain::Surface;
		uint8 BlendModes = 7;
		uint32 PermutationId = 0;
		const FVertexFactoryType* (*VertexFactory)() = nullptr;
	};
	// Detached policy input; workers never consult authored objects or UI state.
	struct FMaterialShaderPermutationParameters : FShaderPermutationParameters
	{
		EMaterialDomain Domain = EMaterialDomain::Surface;
		FMaterialStaticProperties Properties;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		bool bIncludeEditorPasses = true;
	};
	struct FMaterialCompilerEnvironment;
	ENGINE_API auto GetMaterialShaderOutputLayout(const FMaterialShaderEntryDescriptor& Descriptor,
		const FMaterialStaticProperties& Properties) -> FMaterialRenderLayoutIdentity;
	ENGINE_API auto GetMaterialShaderEntries() -> std::vector<FMaterialShaderEntryDescriptor>;
	ENGINE_API auto FindMaterialShaderEntry(std::string_view ShaderType) -> const FMaterialShaderEntryDescriptor*;
	ENGINE_API auto SelectMaterialShaderEntries(const FMaterialStaticProperties& Properties,
		bool bIncludeEditorPasses = true, EMaterialDomain Domain = EMaterialDomain::Surface,
		const FMaterialCompilerEnvironment* Environment = nullptr) -> FMaterialShaderEntrySet;
	ENGINE_API auto GetMaterialCompiledEntryPoints() -> std::vector<std::string_view>;
	ENGINE_API auto GetMaterialShaderCompilationOptions(std::string_view ShaderType, const FMaterialStaticProperties& Properties,
		EMaterialDomain Domain, const FMaterialCompilerEnvironment& Environment) -> FShaderCompileOptions;
	inline auto GetDefaultMaterialShaderEntries() -> FMaterialShaderEntrySet { return SelectMaterialShaderEntries({}); }

	struct FMaterialCompilerDependency
	{
		std::string VirtualPath;
		FXxHash128 ContentHash;

		auto operator==(const FMaterialCompilerDependency&) const
			-> bool = default;
	};

	struct FMaterialCompilerEnvironment
	{
		struct FStaticBoolValue
		{
			FGuid DeclarationId;
			bool Value = false;
			auto operator<=>(const FStaticBoolValue&) const = default;
		};
		std::string CompilerIdentity;
		uint64 BuilderVersion = GetMaterialBuildVersion();
		uint32 AdapterVersion = GetMaterialCompileAdapterVersion();
		std::string Target = "vulkan-spirv-1.5";
		uint32 PassContractVersion = CurrentMaterialPassContractVersion;
		bool bIncludeEditorPasses = true;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		std::vector<FStaticBoolValue> StaticBools;
		std::vector<FMaterialCompilerDependency> Dependencies;
		FMaterialCompilerResourceLimits ResourceLimits;

		auto operator==(const FMaterialCompilerEnvironment&) const
			-> bool = default;
	};

	namespace MIR
	{
		struct FSwizzle
		{
			uint8 Length = 0;
			std::array<uint8, 4> Components{};
			auto operator==(const FSwizzle&) const -> bool = default;
		};

		// Only the selected immediate payload is present. Operations without immediates
		// use monostate; opcode/payload agreement is checked before encoding/generation.
		struct FCollectionParameter
		{
			FGuid CollectionId;
			FGuid ParameterId;
			auto operator==(const FCollectionParameter&) const -> bool = default;
		};
		struct FCustomDataRead
		{
			uint32 Index = 0;
			auto operator==(const FCustomDataRead&) const -> bool = default;
		};
		struct FTextureRead
		{
			EMaterialTextureReadMode Mode = EMaterialTextureReadMode::Implicit;
			auto operator==(const FTextureRead&) const -> bool = default;
		};
		struct FSceneTextureRead
		{
			EMaterialSceneTextureId SceneTextureId = EMaterialSceneTextureId::PostProcessInput0;
			EMaterialSceneTextureFilter Filter = EMaterialSceneTextureFilter::Linear;
			auto operator==(const FSceneTextureRead&) const -> bool = default;
		};
		using FPayload = std::variant<std::monostate, FMaterialProgramLiteral, std::string,
			FSwizzle, FCollectionParameter, FMaterialTransformPayload, FCustomDataRead, FTextureRead, FSceneTextureRead>;

		struct FNode
		{
			EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;
			EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
			EMaterialEvaluationStage LegalStages = EMaterialEvaluationStage::Both;
			EMaterialSpatialKind SpatialKind = EMaterialSpatialKind::None;
			EMaterialCoordinateSpace CoordinateSpace = EMaterialCoordinateSpace::None;
			uint8 ScalarBroadcastMask = 0;
			std::vector<uint32> Inputs;
			FPayload Payload;

			auto GetLiteral() const -> FMaterialProgramLiteral
			{
				const auto* Value = std::get_if<FMaterialProgramLiteral>(&Payload);
				return Value ? *Value : FMaterialProgramLiteral{};
			}
			auto GetParameterId() const -> FMaterialFieldKey
			{
				const auto* Name = std::get_if<std::string>(&Payload);
				return Name ? FMaterialFieldKey(*Name) : FMaterialFieldKey{};
			}
			auto GetSwizzle() const -> FSwizzle
			{
				const auto* Value = std::get_if<FSwizzle>(&Payload);
				return Value ? *Value : FSwizzle{};
			}
			auto GetCollectionParameter() const -> FCollectionParameter
			{
				const auto* Value = std::get_if<FCollectionParameter>(&Payload);
				return Value ? *Value : FCollectionParameter{};
			}
			auto GetTransform() const -> FMaterialTransformPayload
			{
				const auto* Value = std::get_if<FMaterialTransformPayload>(&Payload);
				return Value ? *Value : FMaterialTransformPayload{};
			}
			auto GetTextureReadMode() const -> EMaterialTextureReadMode
			{
				const auto* Read = std::get_if<FTextureRead>(&Payload);
				return Read ? Read->Mode : EMaterialTextureReadMode::Implicit;
			}
			auto GetSceneTextureRead() const -> FSceneTextureRead
			{
				const auto* Read = std::get_if<FSceneTextureRead>(&Payload);
				return Read ? *Read : FSceneTextureRead{};
			}
			auto GetSemantics() const -> FMaterialValueSemantics
			{
				return {ResultType, LegalStages, SpatialKind, CoordinateSpace};
			}
			auto HasValidPayload() const -> bool
			{
				switch (Opcode)
				{
				case EMaterialProgramOpcode::SceneTexture:
				case EMaterialProgramOpcode::SceneDepth:
				{
					const auto* Read = std::get_if<FSceneTextureRead>(&Payload);
					return Read && Read->SceneTextureId <= EMaterialSceneTextureId::SceneDepth
						&& Read->Filter <= EMaterialSceneTextureFilter::Linear
						&& (Read->SceneTextureId != EMaterialSceneTextureId::SceneDepth || Read->Filter == EMaterialSceneTextureFilter::Point)
						&& (Opcode != EMaterialProgramOpcode::SceneDepth || Read->SceneTextureId == EMaterialSceneTextureId::SceneDepth);
				}
				case EMaterialProgramOpcode::PrimitiveCustomData:
				case EMaterialProgramOpcode::InstanceCustomData:
				{
					const auto* Read = std::get_if<FCustomDataRead>(&Payload);
					return Read && ResultType <= EMaterialProgramValueType::Float4
						&& Read->Index < 32 && static_cast<uint32>(ResultType) + 1 <= 32 - Read->Index;
				}
				case EMaterialProgramOpcode::TextureSampleCube:
				case EMaterialProgramOpcode::TextureSample2D:
				{
					const auto* Read = std::get_if<FTextureRead>(&Payload);
					return Read ? Read->Mode <= EMaterialTextureReadMode::Grad : std::holds_alternative<std::monostate>(Payload);
				}
				case EMaterialProgramOpcode::Constant: return std::holds_alternative<FMaterialProgramLiteral>(Payload);
				case EMaterialProgramOpcode::Parameter:
				case EMaterialProgramOpcode::TextureParameter: return std::holds_alternative<std::string>(Payload);
				case EMaterialProgramOpcode::CollectionParameter:
					return std::holds_alternative<FCollectionParameter>(Payload);
				case EMaterialProgramOpcode::Swizzle: return std::holds_alternative<FSwizzle>(Payload);
				case EMaterialProgramOpcode::TransformPosition:
				case EMaterialProgramOpcode::TransformDirection:
				case EMaterialProgramOpcode::TransformNormal:
					return std::holds_alternative<FMaterialTransformPayload>(Payload);
				default: return std::holds_alternative<std::monostate>(Payload);
				}
			}

			auto operator==(const FNode&) const -> bool = default;
		};

		struct FModule
		{
			uint32 Version = CurrentVersion;
			EMaterialDomain Domain = EMaterialDomain::Surface;
			std::vector<FNode> Nodes;
			struct FAttributeRoot
			{
				FGuid AttributeID;
				bool bExpression = false;
				uint32 ExpressionIndex = 0;
				EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
				EMaterialEvaluationStage LegalStages = EMaterialEvaluationStage::Pixel;
				EMaterialSpatialKind SpatialKind = EMaterialSpatialKind::None;
				EMaterialCoordinateSpace CoordinateSpace = EMaterialCoordinateSpace::None;
				FMaterialProgramLiteral Literal;
				auto GetSemantics() const -> FMaterialValueSemantics
				{
					return {Type, LegalStages, SpatialKind, CoordinateSpace};
				}
				auto operator==(const FAttributeRoot&) const -> bool = default;
			};
			bool bAggregateAttributes = false;
			uint32 AggregateAttributesIndex = 0;
			std::vector<FAttributeRoot> AttributeRoots;
			std::vector<FAttributeRoot> CustomOutputRoots;
			auto GetRoots() const
			{
				using Span = std::span<const FAttributeRoot>;
				return std::array{Span(AttributeRoots), Span(CustomOutputRoots)} | std::views::join;
			}
			ENGINE_API auto GetCustomOutput(FGuid Id) -> FAttributeRoot&;
			ENGINE_API auto GetCustomOutput(FGuid Id) const -> const FAttributeRoot&;
			ENGINE_API FModule();
			ENGINE_API auto GetAttribute(EMaterialProperty Property) -> FAttributeRoot&;
			ENGINE_API auto GetAttribute(EMaterialProperty Property) const -> const FAttributeRoot&;

			auto operator==(const FModule&) const -> bool = default;
		};

		// Per-input diagnostic metadata, excluded from shared artifact identity and cooked bytes.
		struct FSource
		{
			uint32 ExpressionIndex = 0;
			FGuid NodeId;
			FGuid PortId;
			std::string FunctionAssetPath;
			std::vector<FGuid> CallPath;
			std::optional<uint32> InputIndex;
			std::optional<uint32> UVFieldIndex;
		};

		struct FNormalizationResult
		{
			bool bSucceeded = false;
			FModule IR;
			std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
			std::vector<FMaterialParameterCollectionLayout> ActiveCollections;
			FMaterialRenderLayout Layout;
			FByteBuffer CanonicalBytes;
			FMaterialProgramIdentity Identity;
			std::vector<FMaterialProgramDiagnostic> Diagnostics;
			std::vector<FSource> Sources;

			explicit operator bool() const { return bSucceeded; }
		};

		// Final compiler input: detached typed IR and its binding/environment contract.
		// Authored Program nodes, function call tables, and object references are absent.
		struct FCompilerInput
		{
			FModule IR;
			std::vector<FMaterialCompilerParameterDeclaration> Parameters;
			std::vector<FMaterialParameterCollectionLayout> Collections;
			FMaterialStaticProperties StaticProperties;
			FMaterialCompilerEnvironment Environment;
			std::vector<FSource> Sources;
		};
	}

	// Keeps each normalized source paired with its requesting-input ordinal.
	struct FMaterialTranslationSource
	{
		MIR::FSource Source;
		uint32 Ordinal = 0;
	};

	struct FMaterialProgramRequirements
	{
		bool bMaterialView = false;
		bool bMaterialPrimitive = false;
		bool bCameraPosition = false;
		bool bViewport = false;
		bool bViewTransforms = false;
		bool bObjectTransforms = false;
		bool bBoundsCenter = false;
		bool bTangentFrame = false;
		bool bVertexNormal = false;
		auto operator==(const FMaterialProgramRequirements&) const -> bool = default;
	};

	// Logical renderer input only; Surface entries currently require none.
	struct FMaterialSceneTextureRequirement
	{
		EMaterialSceneTextureId InputId;
		EMaterialSceneTextureFilter Filter;
		bool bRequired = true;
		auto operator==(const FMaterialSceneTextureRequirement&) const -> bool = default;
	};
	// Conservative evaluation closure; reflection may remove, but never add, reads.
	struct FMaterialEntryRequirements
	{
		std::vector<FGuid> AttributeIDs;
		std::vector<FMaterialFieldKey> ParameterIDs;
		std::vector<uint16> ResourceIndices;
		std::vector<FGuid> CollectionIDs;
		std::vector<FMaterialSceneTextureRequirement> SceneTextures;
		std::vector<FMaterialValueSemantics> Interpolators;
		bool bUsesWPO = false;
		bool bUsesMask = false;
		FMaterialProgramRequirements Context;
		auto operator==(const FMaterialEntryRequirements&) const -> bool = default;
	};
	// Owned execution identity and analyzed requirements, independent of worker order.
	struct FMaterialShaderEntryRequest
	{
		FMaterialShaderPermutationIdentity Shader;
		std::optional<FMeshMaterialShaderPermutationIdentity> Mesh;
		FMaterialEntryRequirements Requirements;
		std::vector<FMaterialShaderBinding> Bindings;
		FXxHash128 ParameterSchemaIdentity{};
		FXxHash128 ParameterMapIdentity{};
		auto operator==(const FMaterialShaderEntryRequest&) const -> bool = default;
	};
	// Immutable analyzed module shared by translated, cooked and runtime artifacts.
	struct FMaterialCompilationOutput
	{
		uint32 BindingSchemaVersion = MaterialBindingSchemaVersion;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		bool bIncludeEditorPasses = true;
		EMaterialDomain Domain = EMaterialDomain::Surface;
		FMaterialStaticProperties StaticProperties;
		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		FMaterialRenderLayout Layout;
		FMaterialProgramRequirements Requirements;
		// Schemas only; paths and defaults belong to each requesting material.
		std::vector<FMaterialParameterCollectionLayout> CollectionLayouts;
		std::vector<FMaterialShaderEntryRequest> Entries;
	};
	ENGINE_API auto ValidateMaterialCompilationOutput(const FMaterialCompilationOutput& Output,
		FMaterialProgramIdentity Identity, std::string_view Target, std::span<const FShaderCode> Stages)
		-> FMaterialLayoutValidationResult;
	ENGINE_API auto GetMaterialEntryProperties(EMaterialShaderEntry Entry, const FMaterialStaticProperties& Properties) -> std::vector<EMaterialProperty>;
	ENGINE_API auto BuildMaterialCompilationOutput(const MIR::FModule& IR, const FMaterialRenderLayout& Layout,
		std::span<const FMaterialCompilerParameterDeclaration> Parameters, std::span<const FMaterialParameterCollectionLayout> Collections,
		const FMaterialStaticProperties& Properties, const FMaterialCompilerEnvironment& Environment, FMaterialProgramIdentity Identity) -> FMaterialCompilationOutput;

	enum class EMaterialProgramStatus : uint8 { Ok, Error, Canceled };
	class FMaterialShaderMap;
	// Editor observations are retained separately and absent from cooked programs.
	struct FMaterialProgramDebugData
	{
		MIR::FModule IR;
		std::string GeneratedSource;
	};
	// Immutable program shared by material resources and render proxies.
	// Shader code lives in ShaderMap; defaults are bound from the root material.
	struct FMaterialProgram
	{
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
		std::vector<FMaterialParameterCollectionLayout> ActiveCollections;
		std::shared_ptr<const FMaterialShaderMap> ShaderMap;
		// Bound with the defaults in layout order for this material publication.
		std::vector<FMaterialParameterInfo> FieldParameters;
		FMaterialInstanceParameterSet DefaultParameters;
		auto FindParameterField(const FMaterialParameterInfo& Info) const -> std::optional<uint32>
		{
			const auto It = std::ranges::find(FieldParameters, Info);
			if (It == FieldParameters.end()) return {};
			return static_cast<uint32>(It - FieldParameters.begin());
		}

		ENGINE_API auto GetShaderMap() const -> const FMaterialShaderMap&;
		ENGINE_API auto GetDebugData() const -> const FMaterialProgramDebugData&;
		auto HasShaderMap() const -> bool { return ShaderMap != nullptr; }
		ENGINE_API auto IsValid() const -> bool;
	};

	// Transient compiler completion; publication copies only the resource payload.
	struct FMaterialCompilerResult
	{
		FMaterialProgram Resource;
		EMaterialProgramStatus Status = EMaterialProgramStatus::Error;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		auto GetShaderMap() const -> const FMaterialShaderMap& { return Resource.GetShaderMap(); }
		auto GetDebugData() const -> const FMaterialProgramDebugData& { return Resource.GetDebugData(); }
		explicit operator bool() const { return Status == EMaterialProgramStatus::Ok; }
	};

	struct FMaterialCompilerSnapshot
	{
		MIR::FCompilerInput Input;
		std::vector<FMaterialFunctionOwnerStamp> FunctionOwners;
	};

	// Captured on the owning thread. A failed capture never publishes a payload.
	struct FMaterialCompilerSnapshotResult
	{
		std::optional<FMaterialCompilerSnapshot> Snapshot;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return Snapshot.has_value(); }
	};

	[[nodiscard]] ENGINE_API auto SnapshotMaterialCompilerInput(
		const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment)
		-> FMaterialCompilerSnapshotResult;
	ENGINE_API auto AreMaterialFunctionOwnersCurrent(std::span<const FMaterialFunctionOwnerStamp> Owners) -> bool;

	[[nodiscard]] ENGINE_API auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment,
		std::optional<FShaderTargetIdentity> Target = std::nullopt)
		-> std::expected<void, std::string>;

	namespace MIR
	{
		ENGINE_API auto Normalize(const FCompilerInput& Input) -> FNormalizationResult;
		[[nodiscard]] ENGINE_API auto Validate(const FModule& IR,
			const FMaterialRenderLayout& Layout) -> FMaterialProgramValidationResult;
		[[nodiscard]] ENGINE_API auto Validate(const FModule& IR,
			std::span<const FMaterialCompilerParameterDeclaration> Parameters) -> FMaterialProgramValidationResult;

		[[nodiscard]] ENGINE_API auto EncodeCanonical(
			const FModule& IR,
			FByteBuffer& OutBytes) -> std::expected<void, std::string>;
	}

	ENGINE_API auto BuildMaterialProgramIdentity(const FMaterialStaticProperties& StaticProperties,
		const FMaterialCompilerEnvironment& Environment, std::span<const FMaterialParameterCollectionLayout> Collections,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout, EMaterialDomain Domain = EMaterialDomain::Surface) -> FMaterialProgramIdentity;
	ENGINE_API auto BuildMaterialProgramIdentity(const MIR::FCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity;
	struct FMaterialSourceGenerationResult
	{
		bool bSucceeded = false;
		std::string Source;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return bSucceeded; }
	};

	[[nodiscard]] ENGINE_API auto ValidateMaterialProgram(
		const FMaterialProgram& Result) -> FMaterialLayoutValidationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialCompilerResult(
		const FMaterialCompilerResult& Result) -> FMaterialLayoutValidationResult;
	// Aggregate authoring must be lowered by MIR::Normalize before source generation.
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const MIR::FModule& IR, const FMaterialRenderLayout& Layout) -> FMaterialSourceGenerationResult;
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const MIR::FModule& IR, const FMaterialRenderLayout& Layout,
		std::span<const FMaterialParameterCollectionLayout> Collections)
		-> FMaterialSourceGenerationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialCompiledStages(
		std::span<const FShaderCode> Stages, const FMaterialRenderLayout& Layout,
		const FMaterialCompilerResourceLimits& Limits = {},
		std::span<const FMaterialParameterCollectionLayout> Collections = {},
		const FMaterialShaderEntrySet& RequiredEntries = GetDefaultMaterialShaderEntries())
		-> FMaterialLayoutValidationResult;
	[[nodiscard]] ENGINE_API auto GenerateMaterialProgramSlang(
		const MIR::FModule& IR) -> FMaterialSourceGenerationResult;
	namespace MIR
	{
		ENGINE_API auto Compile(const FCompilerInput& Input,
			bool bForceRecompile = false) -> FMaterialCompilerResult;
	}
}

template<>
struct std::hash<Durin::FMaterialProgramIdentity>
{
	auto operator()(const Durin::FMaterialProgramIdentity& Identity) const
		noexcept -> size_t
	{
		return std::hash<Durin::FXxHash128>{}(Identity.Digest);
	}
};
