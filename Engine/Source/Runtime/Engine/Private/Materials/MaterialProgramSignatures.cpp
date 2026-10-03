#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	auto GetMaterialSampleOutputs(EMaterialProgramOpcode Opcode) -> std::span<const FMaterialSampleOutputDefinition>
	{
		using Output = EMaterialSampleOutput;
		using Type = EMaterialProgramValueType;
		static constexpr std::array Definitions{
			FMaterialSampleOutputDefinition{Output::RGB, "RGB", Type::Float3},
			FMaterialSampleOutputDefinition{Output::R, "R", Type::Float, 0},
			FMaterialSampleOutputDefinition{Output::G, "G", Type::Float, 1},
			FMaterialSampleOutputDefinition{Output::B, "B", Type::Float, 2},
			FMaterialSampleOutputDefinition{Output::A, "A", Type::Float, 3},
			FMaterialSampleOutputDefinition{Output::RGBA, "RGBA", Type::Float4},
			FMaterialSampleOutputDefinition{Output::Texture, "Texture", Type::Texture2D},
		};
		switch (Opcode)
		{
		case EMaterialProgramOpcode::TextureSample2D: return std::span{Definitions}.first(Definitions.size() - 1);
		case EMaterialProgramOpcode::TextureSampleParameter2D: return Definitions;
		default: return {};
		}
	}

	auto FindMaterialSampleOutput(EMaterialProgramOpcode Opcode, uint8 OutputIndex) -> const FMaterialSampleOutputDefinition*
	{
		for (const auto& Output : GetMaterialSampleOutputs(Opcode))
			if (static_cast<uint8>(Output.Id) == OutputIndex) return &Output;
		return nullptr;
	}

	auto IsMaterialSamplingNode(EMaterialProgramOpcode Opcode) -> bool
	{
		return Opcode == EMaterialProgramOpcode::TextureSample2D
			|| Opcode == EMaterialProgramOpcode::TextureSampleParameter2D;
	}

	auto GetMaterialProgramNodeSignature(
		EMaterialProgramOpcode Opcode, EMaterialProgramValueType ResultType)
		-> std::optional<FMaterialProgramNodeSignature>
	{
		using Type = EMaterialProgramValueType;
		static constexpr std::array Types{
			Type::Float, Type::Float2, Type::Float3, Type::Float4,
			Type::Texture2D, Type::Surface, Type::StaticBool};
		const auto One = [](Type Value) -> std::span<const Type> {
			return {&Types[static_cast<size_t>(Value)], 1};
		};
		const bool bNumeric = ResultType >= Type::Float && ResultType <= Type::Float4;
		FMaterialProgramNodeSignature Signature;
		const auto Same = [&](uint8 Count, Type InputType) {
			Signature.InputCount = Count;
			std::fill_n(Signature.Inputs.begin(), Count, One(InputType));
		};
		switch (Opcode)
		{
		case EMaterialProgramOpcode::VertexInterpolator:
			if (!bNumeric) return std::nullopt;
			Same(1, ResultType);
			break;
		case EMaterialProgramOpcode::StaticBool:
			if (ResultType != Type::StaticBool) return std::nullopt;
			break;
		case EMaterialProgramOpcode::StaticSwitch:
			if (!bNumeric) return std::nullopt;
			Signature.InputCount = 3;
			Signature.Inputs[0] = One(Type::StaticBool);
			Signature.Inputs[1] = Signature.Inputs[2] = One(ResultType);
			break;
		case EMaterialProgramOpcode::QualitySwitch:
			if (!bNumeric) return std::nullopt;
			Same(3, ResultType);
			break;
		case EMaterialProgramOpcode::FeatureLevelSwitch:
			if (!bNumeric) return std::nullopt;
			Same(4, ResultType);
			break;
		case EMaterialProgramOpcode::WorldPosition:
		case EMaterialProgramOpcode::CameraPosition:
		case EMaterialProgramOpcode::CameraVector:
		case EMaterialProgramOpcode::ObjectPosition:
		case EMaterialProgramOpcode::VertexNormal:
			if (ResultType != Type::Float3) return std::nullopt;
			break;
		case EMaterialProgramOpcode::ScreenPosition:
		case EMaterialProgramOpcode::ViewSize:
			if (ResultType != Type::Float2) return std::nullopt;
			break;
		case EMaterialProgramOpcode::Time:
			if (ResultType != Type::Float) return std::nullopt;
			break;
		case EMaterialProgramOpcode::TransformPosition:
		case EMaterialProgramOpcode::TransformDirection:
		case EMaterialProgramOpcode::TransformNormal:
			if (ResultType != Type::Float3) return std::nullopt;
			Same(1, Type::Float3);
			break;
		case EMaterialProgramOpcode::Parameter:
			if (ResultType != Type::Float && ResultType != Type::Float4) return std::nullopt;
			break;
		case EMaterialProgramOpcode::CollectionParameter:
			if (!bNumeric) return std::nullopt;
			break;
		case EMaterialProgramOpcode::Constant:
			if (!bNumeric) return std::nullopt;
			break;
		case EMaterialProgramOpcode::TextureParameter:
			if (ResultType != Type::Texture2D) return std::nullopt;
			break;
		case EMaterialProgramOpcode::UVChannel:
			if (ResultType != Type::Float2) return std::nullopt;
			Same(1, Type::Float);
			break;
		case EMaterialProgramOpcode::TextureCoordinates:
			if (ResultType != Type::Float2) return std::nullopt;
			Same(1, Type::Float);
			break;
		case EMaterialProgramOpcode::TextureSampleParameter2D:
			if (ResultType != Type::Float4) return std::nullopt;
			Same(1, Type::Float2);
			break;
		case EMaterialProgramOpcode::MakeSurface:
			if (ResultType != Type::Surface) return std::nullopt;
			Signature.InputCount = 8;
			for (uint8 Index = 0; Index < Signature.InputCount; ++Index)
				Signature.Inputs[Index] = One(GetMaterialSurfaceOutputType(
					static_cast<EMaterialSurfaceOutput>(Index)));
			break;
		case EMaterialProgramOpcode::GetSurfaceAttributes:
		case EMaterialProgramOpcode::SetSurfaceAttributes:
			if (ResultType != Type::Surface) return std::nullopt;
			Same(1, Type::Surface);
			break;
		case EMaterialProgramOpcode::TextureSample2D:
			if (ResultType != Type::Float4) return std::nullopt;
			Signature.InputCount = 2;
			Signature.Inputs[0] = One(Type::Texture2D);
			Signature.Inputs[1] = One(Type::Float2);
			break;
		case EMaterialProgramOpcode::BlendNormalsRNM:
			if (ResultType != Type::Float3) return std::nullopt;
			Same(2, Type::Float3);
			break;
		case EMaterialProgramOpcode::Dot:
		case EMaterialProgramOpcode::Distance:
			if (ResultType != Type::Float) return std::nullopt;
			Signature.InputCount = 2;
			Signature.Inputs[0] = Signature.Inputs[1] = std::span(Types).subspan(1, 3);
			break;
		case EMaterialProgramOpcode::Length:
			if (ResultType != Type::Float) return std::nullopt;
			Signature.InputCount = 1;
			Signature.Inputs[0] = std::span(Types).subspan(1, 3);
			break;
		case EMaterialProgramOpcode::Cross:
			if (ResultType != Type::Float3) return std::nullopt;
			Same(2, Type::Float3);
			break;
		case EMaterialProgramOpcode::Reflect:
			if (!bNumeric || ResultType == Type::Float) return std::nullopt;
			Same(2, ResultType);
			break;
		case EMaterialProgramOpcode::Add:
		case EMaterialProgramOpcode::Subtract:
		case EMaterialProgramOpcode::Multiply:
		case EMaterialProgramOpcode::Divide:
		case EMaterialProgramOpcode::Minimum:
		case EMaterialProgramOpcode::Maximum:
			if (!bNumeric) return std::nullopt;
			Same(2, ResultType);
			break;
		case EMaterialProgramOpcode::Normalize:
			if (ResultType == Type::Float) return std::nullopt;
			[[fallthrough]];
		case EMaterialProgramOpcode::Negate:
		case EMaterialProgramOpcode::OneMinus:
		case EMaterialProgramOpcode::Absolute:
		case EMaterialProgramOpcode::Saturate:
		case EMaterialProgramOpcode::Sine:
		case EMaterialProgramOpcode::Cosine:
		case EMaterialProgramOpcode::Sqrt:
		case EMaterialProgramOpcode::Exp:
		case EMaterialProgramOpcode::Log:
		case EMaterialProgramOpcode::Floor:
		case EMaterialProgramOpcode::Ceil:
		case EMaterialProgramOpcode::Round:
		case EMaterialProgramOpcode::Frac:
		case EMaterialProgramOpcode::Sign:
			if (!bNumeric) return std::nullopt;
			Same(1, ResultType);
			break;
		case EMaterialProgramOpcode::Pow:
		case EMaterialProgramOpcode::Fmod:
		case EMaterialProgramOpcode::Step:
			if (!bNumeric) return std::nullopt;
			Same(2, ResultType);
			break;
		case EMaterialProgramOpcode::SmoothStep:
			if (!bNumeric) return std::nullopt;
			Same(3, ResultType);
			break;
		case EMaterialProgramOpcode::Clamp:
		case EMaterialProgramOpcode::Lerp:
			if (!bNumeric) return std::nullopt;
			Same(3, ResultType);
			if (Opcode == EMaterialProgramOpcode::Lerp) Signature.Inputs[2] = One(Type::Float);
			break;
		case EMaterialProgramOpcode::MakeFloat2:
		case EMaterialProgramOpcode::MakeFloat3:
		case EMaterialProgramOpcode::MakeFloat4:
		{
			const uint8 Width = static_cast<uint8>(Opcode)
				- static_cast<uint8>(EMaterialProgramOpcode::MakeFloat2) + 2;
			if (ResultType != Types[Width - 1]) return std::nullopt;
			Same(Width, Type::Float);
			break;
		}
		case EMaterialProgramOpcode::Splat2:
		case EMaterialProgramOpcode::Splat3:
		case EMaterialProgramOpcode::Splat4:
		{
			const uint8 Width = static_cast<uint8>(Opcode)
				- static_cast<uint8>(EMaterialProgramOpcode::Splat2) + 2;
			if (ResultType != Types[Width - 1]) return std::nullopt;
			Same(1, Type::Float);
			break;
		}
		case EMaterialProgramOpcode::AppendVector:
			if (!bNumeric || ResultType == Type::Float) return std::nullopt;
			Signature.InputCount = 2;
			Signature.Inputs[0] = Signature.Inputs[1] = std::span(Types).first(3);
			break;
		case EMaterialProgramOpcode::Swizzle:
			if (!bNumeric) return std::nullopt;
			Signature.InputCount = 1;
			Signature.Inputs[0] = std::span(Types).first(4);
			break;
		case EMaterialProgramOpcode::DecodeNormalRG:
			if (ResultType != Type::Float3) return std::nullopt;
			Same(1, Type::Float2);
			break;
		default:
			return std::nullopt;
		}
		return Signature;
	}

	namespace
	{
		auto IsSpatial(const FMaterialValueSemantics& Value) -> bool
		{
			return Value.Kind != EMaterialSpatialKind::None;
		}

		auto SameSpatial(const FMaterialValueSemantics& Left,
			const FMaterialValueSemantics& Right) -> bool
		{
			return Left.Kind == Right.Kind && Left.Space == Right.Space;
		}

		auto WithStages(FMaterialValueSemantics Value,
			std::span<const FMaterialValueSemantics> Inputs,
			EMaterialEvaluationStage Operation = EMaterialEvaluationStage::Both)
			-> std::optional<FMaterialValueSemantics>
		{
			Value.Stages = Operation;
			for (const auto& Input : Inputs)
				Value.Stages = IntersectMaterialStages(Value.Stages, Input.Stages);
			if (Value.Stages == EMaterialEvaluationStage::None
				|| !IsValidMaterialValueSemantics(Value)) return std::nullopt;
			return Value;
		}
	}

	auto ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType ResultType,
		std::span<const FMaterialValueSemantics> Inputs,
		const FMaterialTransformPayload* Transform)
		-> std::optional<FMaterialValueSemantics>
	{
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, ResultType);
		if (!Signature || Inputs.size() != Signature->InputCount) return std::nullopt;
		for (size_t Index = 0; Index < Inputs.size(); ++Index)
			if (!IsValidMaterialValueSemantics(Inputs[Index])
				|| (!std::ranges::contains(Signature->Inputs[Index], Inputs[Index].Type)
					&& !(MaterialNumericInputAllowsScalarBroadcast(Opcode, static_cast<uint32>(Index))
						&& Inputs[Index].Type == EMaterialProgramValueType::Float
						)))
				return std::nullopt;

		FMaterialValueSemantics Result{.Type = ResultType};
		using OpcodeType = EMaterialProgramOpcode;
		using Kind = EMaterialSpatialKind;
		using Space = EMaterialCoordinateSpace;
		using Stage = EMaterialEvaluationStage;
		const auto NonSpatial = [&](Stage Operation = Stage::Both) {
			if (std::ranges::any_of(Inputs, IsSpatial))
				return std::optional<FMaterialValueSemantics>{};
			return WithStages(Result, Inputs, Operation);
		};

		switch (Opcode)
		{
		case OpcodeType::VertexInterpolator:
			if (!MaterialStagesContain(Inputs[0].Stages, Stage::Vertex)) return std::nullopt;
			Result = Inputs[0];
			Result.Stages = Stage::Pixel;
			return Result;
		case OpcodeType::WorldPosition:
			Result.Kind = Kind::Position; Result.Space = Space::World;
			return WithStages(Result, Inputs);
		case OpcodeType::CameraPosition:
		case OpcodeType::ObjectPosition:
			Result.Kind = Kind::Position; Result.Space = Space::World;
			return WithStages(Result, Inputs);
		case OpcodeType::CameraVector:
			Result.Kind = Kind::Direction; Result.Space = Space::World;
			return WithStages(Result, Inputs, Stage::Pixel);
		case OpcodeType::VertexNormal:
			Result.Kind = Kind::Normal; Result.Space = Space::World;
			return WithStages(Result, Inputs, Stage::Vertex);
		case OpcodeType::ScreenPosition:
			Result.Kind = Kind::ScreenCoordinate; Result.Space = Space::Screen;
			return WithStages(Result, Inputs, Stage::Pixel);
		case OpcodeType::ViewSize:
			return WithStages(Result, Inputs);
		case OpcodeType::Time:
		case OpcodeType::Constant:
		case OpcodeType::Parameter:
		case OpcodeType::CollectionParameter:
		case OpcodeType::TextureParameter:
			return WithStages(Result, Inputs);
		case OpcodeType::UVChannel:
		case OpcodeType::TextureCoordinates:
			return NonSpatial();
		case OpcodeType::TextureSample2D:
		case OpcodeType::TextureSampleParameter2D:
			return NonSpatial(Stage::Pixel);
		case OpcodeType::DecodeNormalRG:
			if (IsSpatial(Inputs[0])) return std::nullopt;
			Result.Kind = Kind::Normal; Result.Space = Space::Tangent;
			return WithStages(Result, Inputs, Stage::Pixel);
		case OpcodeType::BlendNormalsRNM:
			if (Inputs[0].Kind != Kind::Normal || Inputs[0].Space != Space::Tangent
				|| !SameSpatial(Inputs[0], Inputs[1])) return std::nullopt;
			Result.Kind = Kind::Normal; Result.Space = Space::Tangent;
			return WithStages(Result, Inputs, Stage::Pixel);
		case OpcodeType::Dot:
			if (Inputs[0].Type != Inputs[1].Type) return std::nullopt;
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if ((Inputs[0].Kind != Kind::Direction && Inputs[0].Kind != Kind::Normal)
				|| (Inputs[1].Kind != Kind::Direction && Inputs[1].Kind != Kind::Normal)
				|| Inputs[0].Space != Inputs[1].Space) return std::nullopt;
			return WithStages(Result, Inputs);
		case OpcodeType::Cross:
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if ((Inputs[0].Kind != Kind::Direction && Inputs[0].Kind != Kind::Normal)
				|| (Inputs[1].Kind != Kind::Direction && Inputs[1].Kind != Kind::Normal)
				|| Inputs[0].Space != Inputs[1].Space) return std::nullopt;
			Result.Kind = Kind::Direction; Result.Space = Inputs[0].Space;
			return WithStages(Result, Inputs);
		case OpcodeType::Length:
			if (Inputs[0].Kind == Kind::Position
				|| Inputs[0].Kind == Kind::ScreenCoordinate) return std::nullopt;
			return WithStages(Result, Inputs);
		case OpcodeType::Distance:
			if (Inputs[0].Type != Inputs[1].Type) return std::nullopt;
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if (!SameSpatial(Inputs[0], Inputs[1])
				|| (Inputs[0].Kind != Kind::Position
					&& Inputs[0].Kind != Kind::ScreenCoordinate)) return std::nullopt;
			return WithStages(Result, Inputs);
		case OpcodeType::Reflect:
			if (Inputs[0].Type != Inputs[1].Type) return std::nullopt;
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if (Inputs[0].Kind != Kind::Direction || Inputs[1].Kind != Kind::Normal
				|| Inputs[0].Space != Inputs[1].Space) return std::nullopt;
			Result.Kind = Kind::Direction; Result.Space = Inputs[0].Space;
			return WithStages(Result, Inputs);
		case OpcodeType::Add:
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if (Inputs[0].Space != Inputs[1].Space
				&& IsSpatial(Inputs[0]) && IsSpatial(Inputs[1])) return std::nullopt;
			if (Inputs[0].Kind == Kind::Position && Inputs[1].Kind == Kind::Direction)
				Result = Inputs[0];
			else if (Inputs[0].Kind == Kind::Direction && Inputs[1].Kind == Kind::Position)
				Result = Inputs[1];
			else if (Inputs[0].Kind == Kind::Direction && Inputs[1].Kind == Kind::Direction)
				Result = Inputs[0];
			else if (Inputs[0].Kind == Kind::ScreenCoordinate && Inputs[1].Kind == Kind::None)
				Result = Inputs[0];
			else if (Inputs[1].Kind == Kind::ScreenCoordinate && Inputs[0].Kind == Kind::None)
				Result = Inputs[1];
			else return std::nullopt;
			Result.Type = ResultType;
			return WithStages(Result, Inputs);
		case OpcodeType::Subtract:
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if (Inputs[0].Kind == Kind::Position && Inputs[1].Kind == Kind::Position
				&& Inputs[0].Space == Inputs[1].Space)
				{ Result.Kind = Kind::Direction; Result.Space = Inputs[0].Space; }
			else if (Inputs[0].Kind == Kind::Position && Inputs[1].Kind == Kind::Direction
				&& Inputs[0].Space == Inputs[1].Space) Result = Inputs[0];
			else if (Inputs[0].Kind == Kind::Direction && Inputs[1].Kind == Kind::Direction
				&& Inputs[0].Space == Inputs[1].Space) Result = Inputs[0];
			else if (Inputs[0].Kind == Kind::ScreenCoordinate
				&& Inputs[1].Kind == Kind::ScreenCoordinate) {}
			else if (Inputs[0].Kind == Kind::ScreenCoordinate
				&& Inputs[1].Kind == Kind::None) Result = Inputs[0];
			else return std::nullopt;
			Result.Type = ResultType;
			return WithStages(Result, Inputs);
		case OpcodeType::Multiply:
		case OpcodeType::Divide:
		{
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			const bool LeftScalar = Inputs[0].Type == EMaterialProgramValueType::Float
				&& !IsSpatial(Inputs[0]);
			const bool RightScalar = Inputs[1].Type == EMaterialProgramValueType::Float
				&& !IsSpatial(Inputs[1]);
			if (Opcode == OpcodeType::Divide && !RightScalar) return std::nullopt;
			if (!(LeftScalar ^ RightScalar)) return std::nullopt;
			Result = LeftScalar ? Inputs[1] : Inputs[0]; Result.Type = ResultType;
			return WithStages(Result, Inputs);
		}
		case OpcodeType::Negate:
			if (Inputs[0].Kind == Kind::Position
				|| Inputs[0].Kind == Kind::ScreenCoordinate) return std::nullopt;
			Result = Inputs[0]; Result.Type = ResultType;
			return WithStages(Result, Inputs);
		case OpcodeType::Normalize:
			if (Inputs[0].Kind == Kind::Position
				|| Inputs[0].Kind == Kind::ScreenCoordinate) return std::nullopt;
			Result = Inputs[0]; Result.Type = ResultType;
			return WithStages(Result, Inputs);
		case OpcodeType::Lerp:
			if (IsSpatial(Inputs[2])) return std::nullopt;
			if (!IsSpatial(Inputs[0]) && !IsSpatial(Inputs[1])) return NonSpatial();
			if (!SameSpatial(Inputs[0], Inputs[1])) return std::nullopt;
			Result = Inputs[0]; Result.Type = ResultType;
			return WithStages(Result, Inputs);
		case OpcodeType::Swizzle:
			if (!IsSpatial(Inputs[0])) return NonSpatial();
			// Payload-specific identity checking is performed by the emitter/IR validator.
			return WithStages(Result, Inputs);
		case OpcodeType::MakeSurface:
			if (Inputs.size() != 8) return std::nullopt;
			for (size_t Index = 0; Index < Inputs.size(); ++Index)
			{
				const auto Expected = GetMaterialSurfaceOutputSemantics(
					static_cast<EMaterialSurfaceOutput>(Index));
				if (Inputs[Index].Type != Expected.Type
					|| Inputs[Index].Kind != Expected.Kind
					|| Inputs[Index].Space != Expected.Space
					|| !MaterialStagesContain(Inputs[Index].Stages, Stage::Pixel))
					return std::nullopt;
			}
			return WithStages(Result, Inputs, Stage::Pixel);
		case OpcodeType::GetSurfaceAttributes:
		case OpcodeType::SetSurfaceAttributes:
			return WithStages(Result, Inputs, Stage::Pixel);
		case OpcodeType::FunctionInput:
		case OpcodeType::FunctionOutput:
		case OpcodeType::FunctionCall:
			return WithStages(Result, Inputs);
		case OpcodeType::TransformPosition:
		case OpcodeType::TransformDirection:
		case OpcodeType::TransformNormal:
		{
			if (!Transform || Transform->Source == Transform->Destination
				|| Transform->Source == Space::None || Transform->Destination == Space::None
				|| Transform->Source == Space::Screen || Transform->Destination == Space::Screen)
				return std::nullopt;
			const Kind Expected = Opcode == OpcodeType::TransformPosition ? Kind::Position
				: Opcode == OpcodeType::TransformDirection ? Kind::Direction : Kind::Normal;
			if (Inputs[0].Kind != Expected || Inputs[0].Space != Transform->Source
				|| (Expected == Kind::Position && (Transform->Source == Space::Tangent
					|| Transform->Destination == Space::Tangent))) return std::nullopt;
			Result.Kind = Expected; Result.Space = Transform->Destination;
			const bool bTangent = Transform->Source == Space::Tangent
				|| Transform->Destination == Space::Tangent;
			return WithStages(Result, Inputs, bTangent ? Stage::Pixel : Stage::Both);
		}
		case OpcodeType::Minimum:
		case OpcodeType::Maximum:
		case OpcodeType::OneMinus:
		case OpcodeType::Absolute:
		case OpcodeType::Saturate:
		case OpcodeType::Clamp:
		case OpcodeType::Sine:
		case OpcodeType::Cosine:
		case OpcodeType::Pow:
		case OpcodeType::Sqrt:
		case OpcodeType::Exp:
		case OpcodeType::Log:
		case OpcodeType::Floor:
		case OpcodeType::Ceil:
		case OpcodeType::Round:
		case OpcodeType::Frac:
		case OpcodeType::Fmod:
		case OpcodeType::Step:
		case OpcodeType::SmoothStep:
		case OpcodeType::Sign:
		case OpcodeType::MakeFloat2:
		case OpcodeType::MakeFloat3:
		case OpcodeType::MakeFloat4:
		case OpcodeType::Splat2:
		case OpcodeType::Splat3:
		case OpcodeType::Splat4:
		case OpcodeType::AppendVector:
			return NonSpatial();
		case OpcodeType::StaticBool:
		case OpcodeType::StaticSwitch:
		case OpcodeType::QualitySwitch:
		case OpcodeType::FeatureLevelSwitch:
			// Authoring-only operations must be eliminated before IR validation.
			return std::nullopt;
		}
		return std::nullopt;
	}
}
