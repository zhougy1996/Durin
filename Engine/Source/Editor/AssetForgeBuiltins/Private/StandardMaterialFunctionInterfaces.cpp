#include "AssetForge/Builtins/StandardMaterialFunctions.h"

namespace Durin::AssetForge::Builtins
{
	auto GetStandardMaterialFunctionInterface(EStandardMaterialFunction Function)
		-> FMaterialFunctionSignature
	{
		using Entry = EStandardMaterialFunction;
		using Type = EMaterialProgramValueType;
		using Kind = EMaterialFunctionDefaultKind;
		FMaterialFunctionSignature Result;
		const auto InputFamily = Function == Entry::StandardPBR_ORM ? Entry::StandardPBR : Function;
		const auto Numeric = [](float X, float Y = 0, float Z = 0) -> FMaterialFunctionDefault {
			return {.Kind = Kind::Numeric, .Numeric = {.X = X, .Y = Y, .Z = Z}};
		};
		const auto Texture = [](EMaterialTextureFallback Fallback = EMaterialTextureFallback::White)
			-> FMaterialFunctionDefault { return {.Kind = Kind::Texture, .TextureFallback = Fallback}; };
		const auto Input = [&](uint32 Slot, std::string Name, Type ValueType,
			FMaterialFunctionDefault Default, bool bAdvanced = false) {
			Result.Inputs.push_back({.Id = StandardMaterialPortId(InputFamily, Slot), .Type = ValueType,
				.Name = std::move(Name), .DisplayOrder = static_cast<int32>(Result.Inputs.size()),
				.bAdvanced = bAdvanced, .Default = std::move(Default)});
		};
		const auto Output = [&](uint32 Slot, std::string Name, Type ValueType) {
			Result.Outputs.push_back({.Id = StandardMaterialPortId(Function, Slot), .Type = ValueType,
				.Name = std::move(Name), .DisplayOrder = static_cast<int32>(Result.Outputs.size())});
		};
		switch (Function)
		{
		case Entry::UVTransform:
			Input(1, "UV", Type::Float2, {.Kind = Kind::UV0});
			Input(2, "Scale", Type::Float2, Numeric(1, 1));
			Input(3, "Offset", Type::Float2, Numeric(0, 0));
			Input(4, "Rotation", Type::Float, Numeric(0));
			Output(100, "UV", Type::Float2);
			break;
		case Entry::SampleNormal:
		case Entry::SampleORM:
			Input(1, "Texture", Type::Texture2D, Texture(Function == Entry::SampleNormal
				? EMaterialTextureFallback::FlatRGNormal : EMaterialTextureFallback::White));
			Input(2, "UV", Type::Float2, {.Kind = Kind::UV0});
			if (Function == Entry::SampleNormal)
			{
				Input(3, "Strength", Type::Float, Numeric(1));
				Input(4, "Normal", Type::Float3, Numeric(0, 0, 1));
				Output(100, "Normal", Type::Float3);
			}
			else
			{
				Output(100, "Occlusion", Type::Float);
				Output(101, "Roughness", Type::Float);
				Output(102, "Metallic", Type::Float);
			}
			break;
		case Entry::StandardPBR:
		case Entry::StandardPBR_ORM:
		{
			constexpr std::array RoleNames{"BaseColor", "Normal", "Metallic", "Roughness",
				"AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};
			const bool bPacked = Function == Entry::StandardPBR_ORM;
			Input(1, "UV", Type::Float2, {.Kind = Kind::UV0});
			for (uint32 I = 0; I < RoleNames.size(); ++I)
			{
				const auto ValueType = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(I));
				const auto Default = I == 0 ? Numeric(.5f, .5f, .5f) : I == 1 ? Numeric(0, 0, 1)
					: I == 3 ? Numeric(.5f) : I == 4 || I >= 6 ? Numeric(1) : Numeric(0);
				Input(10 + I, RoleNames[I], ValueType, Default, I == 1 || I >= 4);
				if (bPacked && I >= 2 && I <= 4) continue;
				Input(20 + I, std::string(RoleNames[I]) + "Texture", Type::Texture2D,
					Texture(I == 1 ? EMaterialTextureFallback::FlatRGNormal : I == 5
						? EMaterialTextureFallback::Black : EMaterialTextureFallback::White), I >= 2);
				Input(30 + I, std::string(RoleNames[I]) + "UV", Type::Float2,
					{.Kind = Kind::Input, .InputId = StandardMaterialPortId(Entry::StandardPBR, 1)}, true);
			}
			if (bPacked)
			{
				Input(40, "ORMTexture", Type::Texture2D, Texture());
				Input(41, "ORMUV", Type::Float2,
					{.Kind = Kind::Input, .InputId = StandardMaterialPortId(Entry::StandardPBR, 1)}, true);
			}
			Output(100, "Surface", Type::Surface);
			break;
		}
		}
		return Result;
	}
}
