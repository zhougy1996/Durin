#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
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
		case EMaterialProgramOpcode::Constant:
		case EMaterialProgramOpcode::Parameter:
			if (!bNumeric) return std::nullopt;
			break;
		case EMaterialProgramOpcode::TextureParameter:
			if (ResultType != Type::Texture2D) return std::nullopt;
			break;
		case EMaterialProgramOpcode::UVChannel:
			if (ResultType != Type::Float2) return std::nullopt;
			Same(1, Type::Float);
			break;
		case EMaterialProgramOpcode::MakeSurface:
			if (ResultType != Type::Surface) return std::nullopt;
			Signature.InputCount = 8;
			for (uint8 Index = 0; Index < Signature.InputCount; ++Index)
				Signature.Inputs[Index] = One(GetMaterialSurfaceOutputType(
					static_cast<EMaterialSurfaceOutput>(Index)));
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
		case EMaterialProgramOpcode::Swizzle:
			if (!bNumeric) return std::nullopt;
			Signature.InputCount = 1;
			Signature.Inputs[0] = std::span(Types).first(4);
			break;
		case EMaterialProgramOpcode::TruncateToFloat:
		case EMaterialProgramOpcode::TruncateToFloat2:
		case EMaterialProgramOpcode::TruncateToFloat3:
		{
			const uint8 Width = static_cast<uint8>(Opcode)
				- static_cast<uint8>(EMaterialProgramOpcode::TruncateToFloat) + 1;
			if (ResultType != Types[Width - 1]) return std::nullopt;
			Signature.InputCount = 1;
			Signature.Inputs[0] = std::span(Types).subspan(Width, 4 - Width);
			break;
		}
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
