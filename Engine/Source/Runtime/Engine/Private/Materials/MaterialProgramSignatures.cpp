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
			Type::Texture2D, Type::Surface};
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
		case EMaterialProgramOpcode::WorldPosition:
			if (ResultType != Type::Float3) return std::nullopt;
			break;
		case EMaterialProgramOpcode::Time:
			if (ResultType != Type::Float) return std::nullopt;
			break;
		case EMaterialProgramOpcode::Parameter:
			if (ResultType != Type::Float && ResultType != Type::Float4) return std::nullopt;
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
			if (!bNumeric) return std::nullopt;
			Same(1, ResultType);
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
}
