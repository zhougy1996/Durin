#pragma once

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "Misc/Guid.h"
#include "EngineAPI.h"

#include "MaterialProgramTypes.gen.h"

#include <span>
#include <string>
#include <vector>

namespace Durin
{
	struct FMaterialParameterDefinition;

	inline constexpr uint32 CurrentMaterialProgramSchemaVersion = 2;
	inline constexpr uint32 LegacyMaterialProgramSchemaVersion = 1;
	inline constexpr uint32 MaterialProgramMaxNodeCount = 256;
	inline constexpr uint32 MaterialProgramMaxLinkCount = 1024;
	inline constexpr uint32 MaterialProgramMaxReferencedParameterCount = 128;
	inline constexpr uint32 MaterialProgramMaxNodeInputCount = 8;
	inline constexpr uint32 MaterialProgramMaxDepth = 64;
	inline constexpr uint32 MaterialProgramMaxDisplayNameBytes = 128;
	inline constexpr uint32 MaterialProgramMaxStringBytes = 16 * 1024;
	inline constexpr uint32 MaterialProgramMaxCanonicalBytes = 1024 * 1024;
	inline constexpr uint32 MaterialProgramMaxDiagnosticCount = 64;
	inline constexpr uint32 MaterialProgramMaxDiagnosticMessageBytes = 512;
	inline constexpr uint32 CurrentMaterialGraphPresentationSchemaVersion = 1;
	inline constexpr int32 MaterialGraphPresentationCoordinateLimit = 1024 * 1024;

	DENUM()
	enum class EMaterialProgramValueType : uint8
	{
		Float,
		Float2,
		Float3,
		Float4,
		Texture2D,
	};

	DENUM()
	enum class EMaterialProgramOpcode : uint8
	{
		Constant,
		Parameter,
		TextureParameter,
		TextureCoordinate,
		TextureSample2D,
		Add,
		Subtract,
		Multiply,
		Divide,
		Minimum,
		Maximum,
		Negate,
		OneMinus,
		Absolute,
		Saturate,
		Normalize,
		Clamp,
		Lerp,
		MakeFloat2,
		MakeFloat3,
		MakeFloat4,
		Swizzle,
		Splat2,
		Splat3,
		Splat4,
		TruncateToFloat,
		TruncateToFloat2,
		TruncateToFloat3,
		DecodeNormalRG,
		BlendNormalsRNM,
	};

	DENUM()
	enum class EMaterialSurfaceOutput : uint8
	{
		BaseColor,
		Normal,
		Metallic,
		Roughness,
		AmbientOcclusion,
		Emissive,
		Opacity,
		OpacityMask,
	};

	DSTRUCT()
	struct FMaterialProgramLink
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid SourceNodeId;

		DPROPERTY()
		uint8 SourceOutputIndex = 0;

		auto operator==(const FMaterialProgramLink&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialProgramLiteral
	{
		GENERATED_BODY()

		DPROPERTY()
		float X = 0.0f;

		DPROPERTY()
		float Y = 0.0f;

		DPROPERTY()
		float Z = 0.0f;

		DPROPERTY()
		float W = 0.0f;

		auto operator==(const FMaterialProgramLiteral&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialProgramNode
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid Id;

		DPROPERTY()
		EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		std::vector<FMaterialProgramLink> Inputs;

		DPROPERTY()
		FMaterialProgramLiteral Literal;

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		uint8 SwizzleLength = 0;

		DPROPERTY()
		uint8 SwizzleX = 0;

		DPROPERTY()
		uint8 SwizzleY = 0;

		DPROPERTY()
		uint8 SwizzleZ = 0;

		DPROPERTY()
		uint8 SwizzleW = 0;

		DPROPERTY()
		std::string DisplayName;

		auto operator==(const FMaterialProgramNode&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialSurfaceOutputs
	{
		GENERATED_BODY()

		DPROPERTY()
		FMaterialProgramLink BaseColor;

		DPROPERTY()
		FMaterialProgramLink Normal;

		DPROPERTY()
		FMaterialProgramLink Metallic;

		DPROPERTY()
		FMaterialProgramLink Roughness;

		DPROPERTY()
		FMaterialProgramLink AmbientOcclusion;

		DPROPERTY()
		FMaterialProgramLink Emissive;

		DPROPERTY()
		FMaterialProgramLink Opacity;

		DPROPERTY()
		FMaterialProgramLink OpacityMask;

		DPROPERTY()
		FMaterialProgramLiteral BaseColorDefault{0.5f, 0.5f, 0.5f, 0.0f};

		DPROPERTY()
		FMaterialProgramLiteral NormalDefault{0.0f, 0.0f, 1.0f, 0.0f};

		DPROPERTY()
		FMaterialProgramLiteral MetallicDefault{};

		DPROPERTY()
		FMaterialProgramLiteral RoughnessDefault{0.5f, 0.0f, 0.0f, 0.0f};

		DPROPERTY()
		FMaterialProgramLiteral AmbientOcclusionDefault{1.0f, 0.0f, 0.0f, 0.0f};

		DPROPERTY()
		FMaterialProgramLiteral EmissiveDefault{};

		DPROPERTY()
		FMaterialProgramLiteral OpacityDefault{1.0f, 0.0f, 0.0f, 0.0f};

		DPROPERTY()
		FMaterialProgramLiteral OpacityMaskDefault{1.0f, 0.0f, 0.0f, 0.0f};

		auto operator==(const FMaterialSurfaceOutputs&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialProgram
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 SchemaVersion = CurrentMaterialProgramSchemaVersion;

		DPROPERTY()
		std::vector<FMaterialProgramNode> Nodes;

		DPROPERTY()
		FMaterialSurfaceOutputs Outputs;

		auto operator==(const FMaterialProgram&) const -> bool = default;
	};

	// Stores one package-persisted editor position for a live material-program node.
	DSTRUCT()
	struct FMaterialGraphNodePresentation
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid NodeId;

		DPROPERTY()
		int32 X = 0;

		DPROPERTY()
		int32 Y = 0;

		auto operator==(const FMaterialGraphNodePresentation&) const -> bool = default;
	};

	// Owns shared authored graph presentation without participating in shader semantics.
	DSTRUCT()
	struct FMaterialGraphPresentation
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 SchemaVersion = CurrentMaterialGraphPresentationSchemaVersion;

		DPROPERTY()
		std::vector<FMaterialGraphNodePresentation> Nodes;

		auto operator==(const FMaterialGraphPresentation&) const -> bool = default;
	};

	enum class EMaterialProgramDiagnosticCategory : uint8
	{
		Schema,
		Bounds,
		Graph,
		Type,
		Normalization,
		Generation,
		Dependency,
		Compile,
		Reflection,
		Binding,
	};

	enum class EMaterialProgramDiagnosticLocationKind : uint8
	{
		Program,
		Node,
		Input,
		SurfaceOutput,
	};

	struct FMaterialProgramDiagnostic
	{
		EMaterialProgramDiagnosticCategory Category =
			EMaterialProgramDiagnosticCategory::Schema;
		EMaterialProgramDiagnosticLocationKind LocationKind =
			EMaterialProgramDiagnosticLocationKind::Program;
		FGuid NodeId;
		uint32 LocationIndex = 0;
		std::string Message;

		auto operator==(const FMaterialProgramDiagnostic&) const -> bool = default;
	};

	struct FMaterialProgramValidationResult
	{
		bool bSucceeded = false;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

		operator bool() const { return bSucceeded; }
	};

	ENGINE_API auto MakeDefaultMaterialProgram() -> FMaterialProgram;
	ENGINE_API auto MakeLegacyExpandedMaterialProgram() -> FMaterialProgram;
	ENGINE_API auto MakeCanonicalMaterialProgram() -> FMaterialProgram;
	ENGINE_API auto UpgradeMaterialProgramSchema(FMaterialProgram& Program)
		-> bool;
	ENGINE_API auto GetMaterialSurfaceOutputType(EMaterialSurfaceOutput Output)
		-> EMaterialProgramValueType;
	ENGINE_API auto GetMaterialSurfaceOutputLink(
		FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> FMaterialProgramLink&;
	ENGINE_API auto GetMaterialSurfaceOutputLink(
		const FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> const FMaterialProgramLink&;
	ENGINE_API auto GetMaterialSurfaceOutputDefault(
		FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> FMaterialProgramLiteral&;
	ENGINE_API auto GetMaterialSurfaceOutputDefault(
		const FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> const FMaterialProgramLiteral&;
	ENGINE_API auto ValidateMaterialProgram(
		const FMaterialProgram& Program,
		std::span<const FMaterialParameterDefinition> ParameterDefinitions)
		-> FMaterialProgramValidationResult;
	ENGINE_API auto SanitizeMaterialGraphPresentation(
		const FMaterialGraphPresentation& Presentation,
		const FMaterialProgram& Program) -> FMaterialGraphPresentation;
}
