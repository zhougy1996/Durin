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
		const auto Numeric = [](float X, float Y = 0, float Z = 0) -> FMaterialFunctionDefault {
			return {.Kind = Kind::Numeric, .Numeric = {.X = X, .Y = Y, .Z = Z}};
		};
		const auto Texture = [](EMaterialTextureFallback Fallback = EMaterialTextureFallback::White)
			-> FMaterialFunctionDefault { return {.Kind = Kind::Texture, .TextureFallback = Fallback}; };
		const auto Input = [&](uint32 Slot, std::string Name, Type ValueType,
			FMaterialFunctionDefault Default, bool bAdvanced = false) {
			Result.Inputs.push_back({.Id = StandardMaterialPortId(Function, Slot), .Type = ValueType,
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
		}
		return Result;
	}
}
