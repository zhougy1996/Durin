#pragma once

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "Misc/Guid.h"
#include "EngineAPI.h"
#include "Materials/MaterialDiagnostic.h"
#include "Materials/MaterialTypes.h"

#include "MaterialProgramTypes.gen.h"

#include <span>
#include <string>
#include <vector>

namespace Durin
{
	struct FMaterialParameterDefinition;
	struct FMaterialProgramValidationResult;
	enum class EMaterialSurfaceOutput : uint8;

	inline constexpr uint32 MaterialProgramMaxVertexInterpolators = 8;
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
		// Authoring-only selector value. Normalization must eliminate it.
		StaticBool,
	};

	// A value is legal in every stage represented by this mask. Both is not an
	// interpolation request: the consuming root still selects one exact stage.
	DENUM()
	enum class EMaterialEvaluationStage : uint8
	{
		None = 0,
		Vertex = 1,
		Pixel = 2,
		Both = 3,
	};

	DENUM()
	enum class EMaterialSpatialKind : uint8
	{
		None,
		Position,
		Direction,
		Normal,
		ScreenCoordinate,
	};

	DENUM()
	enum class EMaterialCoordinateSpace : uint8
	{
		None,
		Object,
		World,
		View,
		Tangent,
		Screen,
	};

	// Detached semantic value used by authored emission, MIR, functions, and
	// diagnostics. Base shape remains independent from spatial meaning.
	struct FMaterialValueSemantics
	{
		EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
		EMaterialEvaluationStage Stages = EMaterialEvaluationStage::Both;
		EMaterialSpatialKind Kind = EMaterialSpatialKind::None;
		EMaterialCoordinateSpace Space = EMaterialCoordinateSpace::None;

		auto operator==(const FMaterialValueSemantics&) const -> bool = default;
	};

	constexpr auto IntersectMaterialStages(
		EMaterialEvaluationStage Left, EMaterialEvaluationStage Right)
		-> EMaterialEvaluationStage
	{
		return static_cast<EMaterialEvaluationStage>(
			static_cast<uint8>(Left) & static_cast<uint8>(Right));
	}

	constexpr auto MaterialStagesContain(
		EMaterialEvaluationStage Mask, EMaterialEvaluationStage Stage) -> bool
	{
		return (static_cast<uint8>(Mask) & static_cast<uint8>(Stage))
			== static_cast<uint8>(Stage);
	}

	inline auto IsMaterialWorldPositionOffsetSemantics(const FMaterialValueSemantics& Value) -> bool
	{
		return Value.Type == EMaterialProgramValueType::Float3
			&& MaterialStagesContain(Value.Stages, EMaterialEvaluationStage::Vertex)
			&& ((Value.Kind == EMaterialSpatialKind::None && Value.Space == EMaterialCoordinateSpace::None)
				|| ((Value.Kind == EMaterialSpatialKind::Direction || Value.Kind == EMaterialSpatialKind::Normal)
					&& Value.Space == EMaterialCoordinateSpace::World));
	}

	ENGINE_API auto IsValidMaterialValueSemantics(
		const FMaterialValueSemantics& Value) -> bool;
	ENGINE_API auto GetMaterialValueSemanticsName(
		const FMaterialValueSemantics& Value) -> std::string;

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
		CollectionParameter,
		TransformPosition,
		TransformDirection,
		TransformNormal,
		Dot,
		Cross,
		Length,
		Distance,
		Pow,
		Sqrt,
		Exp,
		Log,
		Floor,
		Ceil,
		Round,
		Frac,
		Fmod,
		Step,
		SmoothStep,
		Sign,
		Reflect,
		CameraPosition,
		CameraVector,
		ObjectPosition,
		VertexNormal,
		ScreenPosition,
		ViewSize,
		StaticBool,
		StaticSwitch,
		QualitySwitch,
		FeatureLevelSwitch,
		VertexInterpolator,
	};

	struct FMaterialTransformPayload
	{
		EMaterialCoordinateSpace Source = EMaterialCoordinateSpace::None;
		EMaterialCoordinateSpace Destination = EMaterialCoordinateSpace::None;
		auto operator==(const FMaterialTransformPayload&) const -> bool = default;
	};

	// Numeric authoring nodes infer their width from their operands in the editor.
	inline auto IsMaterialAdaptiveNumeric(EMaterialProgramOpcode Opcode) -> bool
	{
		return (Opcode >= EMaterialProgramOpcode::Add && Opcode <= EMaterialProgramOpcode::Lerp)
			|| Opcode == EMaterialProgramOpcode::Sine || Opcode == EMaterialProgramOpcode::Cosine
			|| (Opcode >= EMaterialProgramOpcode::Pow && Opcode <= EMaterialProgramOpcode::Sign)
			|| Opcode == EMaterialProgramOpcode::Reflect
			|| Opcode == EMaterialProgramOpcode::VertexInterpolator;
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

	inline auto MaterialNumericInputAllowsScalarBroadcast(
		EMaterialProgramOpcode Opcode, uint32 Slot) -> bool
	{
		if (Opcode == EMaterialProgramOpcode::Normalize
			|| Opcode == EMaterialProgramOpcode::BlendNormalsRNM
			|| Opcode == EMaterialProgramOpcode::DecodeNormalRG
			|| Opcode == EMaterialProgramOpcode::TransformPosition
			|| Opcode == EMaterialProgramOpcode::TransformDirection
			|| Opcode == EMaterialProgramOpcode::TransformNormal
			|| (Opcode >= EMaterialProgramOpcode::Dot
				&& Opcode <= EMaterialProgramOpcode::Distance)
			|| Opcode == EMaterialProgramOpcode::Reflect
			|| (Opcode == EMaterialProgramOpcode::Lerp && Slot == 2)
			|| (Opcode == EMaterialProgramOpcode::MakeSurface
				&& Slot == static_cast<uint32>(EMaterialSurfaceOutput::Normal)))
			return false;
		return true;
	}

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

	// Resolves one concrete semantic result and validates every operand. The
	// returned error is deterministic and independent from editor presentation.
	ENGINE_API auto ResolveMaterialProgramNodeSemantics(
		EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType ResultType,
		std::span<const FMaterialValueSemantics> Inputs,
		const FMaterialTransformPayload* Transform = nullptr)
		-> std::optional<FMaterialValueSemantics>;
	ENGINE_API auto GetMaterialSurfaceOutputSemantics(EMaterialSurfaceOutput Output)
		-> FMaterialValueSemantics;

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
		FMaterialError Error;
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

		explicit operator bool() const { return bSucceeded; }
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
