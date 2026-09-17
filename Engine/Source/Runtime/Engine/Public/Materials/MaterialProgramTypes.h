#pragma once

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "Misc/Guid.h"
#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

#include "MaterialProgramTypes.gen.h"

#include <span>
#include <string>
#include <vector>

namespace Durin
{
	struct FMaterialParameterDefinition;
	struct FMaterialProgramValidationResult;
	namespace MaterialParameters
	{
		enum class EMaterialBuiltinParameterKind : uint8;
	}

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
	inline constexpr uint32 CurrentMaterialGraphPresentationSchemaVersion = 2;
	inline constexpr int32 MaterialGraphPresentationCoordinateLimit = 1024 * 1024;

	DENUM()
	enum class EMaterialProgramValueType : uint8
	{
		Float,
		Float2,
		Float3,
		Float4,
		Texture2D,
		Surface,
	};

	DENUM()
	enum class EMaterialProgramOpcode : uint8
	{
		Constant,
		Parameter,
		TextureParameter,
		TextureSample2D = 4,
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
		// Retired channel opcodes 25-27 remain unassigned.
		DecodeNormalRG = 28,
		BlendNormalsRNM,
		UVChannel = 31,
		Sine,
		Cosine,
		MakeSurface,
		FunctionInput,
		FunctionOutput,
		FunctionCall,
		GetSurfaceAttributes,
		SetSurfaceAttributes,
		TextureSampleParameter2D,
		TextureCoordinates,
		AppendVector,
		WorldPosition,
		Time,
	};

	// Numeric authoring nodes infer their width from their operands in the editor.
	inline auto IsMaterialAdaptiveNumeric(EMaterialProgramOpcode Opcode) -> bool
	{
		return (Opcode >= EMaterialProgramOpcode::Add && Opcode <= EMaterialProgramOpcode::Lerp)
			|| Opcode == EMaterialProgramOpcode::Sine || Opcode == EMaterialProgramOpcode::Cosine;
	}

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

	// Describes base pin types only; literal, parameter, and swizzle payloads need validation.
	struct FMaterialProgramNodeSignature
	{
		std::array<std::span<const EMaterialProgramValueType>, MaterialProgramMaxNodeInputCount> Inputs{};
		uint8 InputCount = 0;
	};

	// Returns no signature for an unsupported opcode/result pair. Input spans have static lifetime.
	ENGINE_API auto GetMaterialProgramNodeSignature(
		EMaterialProgramOpcode Opcode, EMaterialProgramValueType ResultType)
		-> std::optional<FMaterialProgramNodeSignature>;

	// Resolves the persistent built-in parameter identity owned by one surface output.
	ENGINE_API auto GetMaterialSurfaceParameterId(
		EMaterialSurfaceOutput Output,
		MaterialParameters::EMaterialBuiltinParameterKind Kind) -> FGuid;

	DSTRUCT()
	struct FMaterialProgramLink
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid SourceNodeId;

		DPROPERTY()
		uint8 SourceOutputIndex = 0;

		DPROPERTY()
		FGuid SourceOutputId;

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

	DENUM()
	enum class EMaterialInputDefaultKind : uint8
	{
		None,
		Literal,
	};

	// Retained numeric input value. Connections override it without erasing author intent.
	DSTRUCT()
	struct FMaterialInputDefault
	{
		GENERATED_BODY()

		DPROPERTY()
		EMaterialInputDefaultKind Kind = EMaterialInputDefaultKind::None;

		DPROPERTY()
		EMaterialProgramValueType Type = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialProgramLiteral Literal;

		auto operator==(const FMaterialInputDefault&) const -> bool = default;
	};

	// A sample's local coordinates, replaced in full by its connected UV expression.


	ENGINE_API auto IsMaterialSamplingNode(EMaterialProgramOpcode Opcode) -> bool;

	// Serialized output identities; retired index 6 must not be reused.
	enum class EMaterialSampleOutput : uint8
	{
		RGBA = 0, RGB = 1, R = 2, G = 3, B = 4, A = 5, Texture = 7,
	};

	struct FMaterialSampleOutputDefinition
	{
		EMaterialSampleOutput Id;
		const char* Name;
		EMaterialProgramValueType Type;
		uint8 FirstComponent = 0;
	};

	// Definitions are in display order, independent of serialized identities.
	ENGINE_API auto GetMaterialSampleOutputs(EMaterialProgramOpcode Opcode)
		-> std::span<const FMaterialSampleOutputDefinition>;
	ENGINE_API auto FindMaterialSampleOutput(EMaterialProgramOpcode Opcode, uint8 OutputIndex)
		-> const FMaterialSampleOutputDefinition*;

	DSTRUCT()
	struct FMaterialSurfaceOutputs
	{
		GENERATED_BODY()

		// Aggregate surface source. Valid only when every per-property link is
		// disconnected; retained defaults remain available after disconnection.
		DPROPERTY()
		FMaterialProgramLink Surface;

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

		DPROPERTY()
		std::string DisplayName;

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
		FGuid PortId;
		std::string FunctionAssetPath;
		std::vector<FGuid> CallPath;
		std::optional<uint32> InputIndex;
		std::optional<uint32> UVFieldIndex;

		auto operator==(const FMaterialProgramDiagnostic&) const -> bool = default;
	};

	struct FMaterialProgramValidationResult
	{
		bool bSucceeded = false;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;

		operator bool() const { return bSucceeded; }
	};

	ENGINE_API auto IsMaterialSurfaceOutputActive(EMaterialSurfaceOutput Output,
		const FMaterialStaticProperties& Properties) -> bool;

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
	ENGINE_API auto SanitizeMaterialGraphPresentation(
		const FMaterialGraphPresentation& Presentation,
		std::span<const FGuid> ExpressionIds) -> FMaterialGraphPresentation;
}
