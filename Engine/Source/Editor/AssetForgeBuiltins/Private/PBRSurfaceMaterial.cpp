#include "AssetForge/Builtins/PBRSurfaceMaterial.h"
#include "MaterialExpressionRecipeBuilder.h"

namespace Durin::AssetForge::Builtins
{
	auto MakePBRSurfaceMaterialMRExpressions() -> FMaterialExpressionRecipe
	{
		using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
		FMaterialExpressionRecipe Recipe;
		Recipe.OutputPosition = {1600, 400};
		Private::FMaterialExpressionRecipeBuilder B{Recipe, 0x7f53711b, 0x48249b21};
		const auto Definitions = MakePBRMaterialParameterDefinitions();
		constexpr std::array<uint8, 8> Channels{1, 1, 4, 3, 2, 1, 5, 2};
		for (uint32 I = 0; I < Channels.size(); ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const int32 Row = static_cast<int32>(I) * 900;
			const auto Parameter = [&](Kind ParameterKind, int32 X, int32 Y) {
				const auto Id = GetMaterialSurfaceParameterId(Role, ParameterKind);
				const auto Definition = std::ranges::find(Definitions, Id, &FMaterialParameterDefinition::Id);
				require(Definition != Definitions.end());
				return B.Parameter(*Definition, I, X, Row + Y, ParameterKind == Kind::UVScale || ParameterKind == Kind::UVOffset
					? EMaterialProgramValueType::Float2 : ParameterKind == Kind::Value ? GetMaterialSurfaceOutputType(Role) : EMaterialProgramValueType::Float4);
			};
			const FMaterialExpressionInput Factor{Parameter(Kind::Value, 640, 0)->Id};
			const FMaterialExpressionInput Channel{Parameter(Kind::UVChannel, -2920, 0)->Id};
			const FMaterialExpressionInput Scale{Parameter(Kind::UVScale, -2920, 130)->Id};
			const FMaterialExpressionInput Offset{Parameter(Kind::UVOffset, -2920, 260)->Id};
			const FMaterialExpressionInput Rotation{Parameter(Kind::UVRotation, -2920, 390)->Id};
			const auto UV = B.TransformUV(I, -2600, Row, Channel, Scale, Offset, Rotation);
			auto* Texture = Cast<DMaterialExpressionTextureSampleParameter2D>(Parameter(Kind::Texture, 0, 0));
			Texture->UV = UV;
			const FMaterialExpressionInput Sample{Texture->Id, Channels[I]};
			auto& Output = B.Output(Recipe.Outputs, I);
			if (I == 1)
			{
				auto* N = B.Add<DMaterialExpressionBlendNormalsRNM>(I, 960, Row);
				N->Base = Factor; N->Detail = Sample; Output = {N->Id};
			}
			else if (I == 5)
			{
				auto* N = B.Add<DMaterialExpressionAdd>(I, 960, Row);
				N->ResultType = GetMaterialSurfaceOutputType(Role); N->A = Factor; N->B = Sample; Output = {N->Id};
			}
			else
			{
				auto* N = B.Add<DMaterialExpressionMultiply>(I, 960, Row);
				N->ResultType = GetMaterialSurfaceOutputType(Role); N->A = Factor; N->B = Sample; Output = {N->Id};
			}
		}
		return Recipe;
	}
}
