#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialCompilationConfiguration.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Materials/MaterialParameterCollection.h"
#include "Shader/MaterialShaderIdentity.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	class DMaterialInterface;

	namespace MIR
	{
		inline constexpr uint32 CurrentVersion = 8;
	}
	// Opaque producer identity; algorithm changes invalidate builds, not DMAT admission.
	ENGINE_API auto GetMaterialBuildVersion() -> uint64;
	ENGINE_API auto GetMaterialCompileAdapterVersion() -> uint32;
	inline constexpr uint32 CurrentMaterialPassContractVersion = 11;
	// Values are the persisted canonical stage-bit order. Append rather than reorder.
	enum class EMaterialShaderEntry : uint8
	{
		Forward, GBuffer, Shadow, HitProxy, LocalVertex, SplineVertex, GPUCullingVertex,
	};
	struct FMaterialShaderEntryDescriptor
	{
		EMaterialShaderEntry Entry;
		std::string_view Name;
		EShaderFrequency Frequency;
		uint8 GeometryUsage;
		uint8 PassUsage;
		bool bEditorOnly = false;
		constexpr auto GetBit() const -> uint8 { return uint8(1u << static_cast<uint8>(Entry)); }
	};
	inline constexpr std::array MaterialShaderEntries{
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::Forward, "FragmentMain", EShaderFrequency::Fragment, 3, 1},
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::GBuffer, "GeometryFragmentMain", EShaderFrequency::Fragment, 7, 2},
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::Shadow, "ShadowFragmentMain", EShaderFrequency::Fragment, 3, 4},
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::HitProxy, "HitProxyFragmentMain", EShaderFrequency::Fragment, 3, 8, true},
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::LocalVertex, "VertexMain", EShaderFrequency::Vertex, 1, 15},
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::SplineVertex, "SplineVertexMain", EShaderFrequency::Vertex, 2, 15},
		FMaterialShaderEntryDescriptor{EMaterialShaderEntry::GPUCullingVertex, "GPUCullingVertexMain", EShaderFrequency::Vertex, 4, 2},
	};
	constexpr auto MaterialShaderEntryBit(EMaterialShaderEntry Entry) -> uint8
	{
		return MaterialShaderEntries[static_cast<uint8>(Entry)].GetBit();
	}
	constexpr auto MaterialShaderStageMask(EShaderFrequency Frequency) -> uint8
	{
		uint8 Mask = 0;
		for (const auto& Entry : MaterialShaderEntries)
			if (Entry.Frequency == Frequency) Mask |= Entry.GetBit();
		return Mask;
	}
	inline constexpr uint8 MaterialFragmentStageMask = MaterialShaderStageMask(EShaderFrequency::Fragment);
	inline constexpr uint8 MaterialVertexStageMask = MaterialShaderStageMask(EShaderFrequency::Vertex);
	inline constexpr uint8 MaterialAllStageMask = MaterialFragmentStageMask | MaterialVertexStageMask;
	inline constexpr auto MaterialCompiledEntryPoints = [] {
		std::array<std::string_view, MaterialShaderEntries.size()> Names;
		for (size_t Index = 0; Index < Names.size(); ++Index) Names[Index] = MaterialShaderEntries[Index].Name;
		return Names;
	}();

	constexpr auto SelectMaterialShaderStages(const FMaterialStaticProperties& Properties,
		bool bIncludeEditorPasses = true) -> uint8
	{
		uint8 Passes = 0;
		for (const auto& Entry : MaterialShaderEntries)
			if (Entry.Frequency == EShaderFrequency::Fragment && (!Entry.bEditorOnly || bIncludeEditorPasses))
				Passes |= Entry.PassUsage;
		Passes &= Properties.PassUsage;
		uint8 Stages = 0;
		for (const auto& Entry : MaterialShaderEntries)
			if ((!Entry.bEditorOnly || bIncludeEditorPasses)
				&& (Properties.GeometryUsage & Entry.GeometryUsage) && (Passes & Entry.PassUsage))
				Stages |= Entry.GetBit();
		return Stages;
	}

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
		using FPayload = std::variant<std::monostate, FMaterialProgramLiteral, FGuid,
			FSwizzle, FCollectionParameter, FMaterialTransformPayload, FCustomDataRead>;

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
			auto GetParameterId() const -> FGuid
			{
				const auto* Value = std::get_if<FGuid>(&Payload);
				return Value ? *Value : FGuid{};
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
			auto GetSemantics() const -> FMaterialValueSemantics
			{
				return {ResultType, LegalStages, SpatialKind, CoordinateSpace};
			}
			auto HasValidPayload() const -> bool
			{
				switch (Opcode)
				{
				case EMaterialProgramOpcode::PrimitiveCustomData:
				case EMaterialProgramOpcode::InstanceCustomData:
				{
					const auto* Read = std::get_if<FCustomDataRead>(&Payload);
					return Read && ResultType <= EMaterialProgramValueType::Float4
						&& Read->Index < 32 && static_cast<uint32>(ResultType) + 1 <= 32 - Read->Index;
				}
				case EMaterialProgramOpcode::Constant: return std::holds_alternative<FMaterialProgramLiteral>(Payload);
				case EMaterialProgramOpcode::Parameter:
				case EMaterialProgramOpcode::TextureParameter: return std::holds_alternative<FGuid>(Payload);
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
			std::vector<FNode> Nodes;
			struct FSurfaceInput
			{
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
				auto operator==(const FSurfaceInput&) const -> bool = default;
			};
			struct FSurfaceRoot
			{
				bool bAggregate = false;
				uint32 AggregateExpressionIndex = 0;
				std::array<FSurfaceInput, 8> Inputs;
				auto operator==(const FSurfaceRoot&) const -> bool = default;
			};
			FSurfaceRoot SurfaceRoot;
			FSurfaceInput WorldPositionOffset{.Type = EMaterialProgramValueType::Float3,
				.LegalStages = EMaterialEvaluationStage::Vertex};

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

	struct FMaterialCompileTimings
	{
		uint64 NormalizationMicroseconds = 0;
		uint64 GenerationMicroseconds = 0;
		uint64 CompilationMicroseconds = 0;
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

	enum class EMaterialProgramStatus : uint8 { Ok, Error, Canceled };
	class FMaterialShaderMap;
	// Editor observations are retained separately and absent from cooked programs.
	struct FMaterialProgramDebugData
	{
		MIR::FModule IR;
		std::string GeneratedSource;
	};
	struct FMaterialCompilerResult
	{
		EMaterialProgramStatus Status = EMaterialProgramStatus::Error;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
		std::vector<FMaterialParameterCollectionLayout> ActiveCollections;
		std::shared_ptr<const FMaterialShaderMap> ShaderMap;
		FMaterialCompileTimings Timings;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

		ENGINE_API auto GetShaderMap() const -> const FMaterialShaderMap&;

		ENGINE_API auto GetDebugData() const -> const FMaterialProgramDebugData&;

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
		-> FMaterialOperationResult;

	namespace MIR
	{
		ENGINE_API auto Normalize(const FCompilerInput& Input) -> FNormalizationResult;
		[[nodiscard]] ENGINE_API auto Validate(const FModule& IR,
			const FMaterialRenderLayout& Layout) -> FMaterialProgramValidationResult;
		[[nodiscard]] ENGINE_API auto Validate(const FModule& IR,
			std::span<const FMaterialCompilerParameterDeclaration> Parameters) -> FMaterialProgramValidationResult;

		[[nodiscard]] ENGINE_API auto EncodeCanonical(
			const FModule& IR,
			FByteBuffer& OutBytes) -> FMaterialOperationResult;
	}

	ENGINE_API auto BuildMaterialProgramIdentity(const FMaterialStaticProperties& StaticProperties,
		const FMaterialCompilerEnvironment& Environment, std::span<const FMaterialParameterCollectionLayout> Collections,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity;
	ENGINE_API auto BuildMaterialProgramIdentity(const MIR::FCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity;
	struct FMaterialSourceGenerationResult
	{
		bool bSucceeded = false;
		std::string Source;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return bSucceeded; }
	};

	[[nodiscard]] ENGINE_API auto ValidateMaterialCompilerResult(
		const FMaterialCompilerResult& Result) -> FMaterialLayoutValidationResult;
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
		uint8 RequiredStages = MaterialAllStageMask)
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
