#pragma once
#include "AssetForge/Builtins/MaterialExpressionRecipe.h"

namespace Durin::AssetForge::Builtins::Private
{
	struct FMaterialExpressionRecipeBuilder
	{
		FMaterialExpressionRecipe& Recipe;
		uint32 GuidA, GuidB;
		template<typename T>
		auto Add(uint32 Role, int32 X, int32 Y) -> T*
		{
			auto* N = NewObject<T>(nullptr, NAME_None);
			N->Id = {GuidA, GuidB, Role, static_cast<uint32>(Recipe.Expressions.size() + 1)};
			Recipe.Expressions.emplace_back(N);
			Recipe.Presentation.Nodes.push_back({N->Id, X, Y});
			return N;
		}
		auto Parameter(const FMaterialParameterDefinition& Definition, uint32 Role, int32 X, int32 Y)
			-> DMaterialExpressionParameter*
		{
			DMaterialExpressionParameter* Parameter = nullptr;
			switch (Definition.Value.GetType())
			{
			case EMaterialParameterType::Scalar:
			{
				auto* N = Add<DMaterialExpressionScalarParameter>(Role, X, Y);
				N->DefaultValue = Definition.Value.GetScalar(); N->bHasRange = Definition.bHasRange;
				N->MinimumValue = Definition.MinimumValue; N->MaximumValue = Definition.MaximumValue;
				Parameter = N; break;
			}
			case EMaterialParameterType::Vector2:
			{
				auto* N = Add<DMaterialExpressionVector2Parameter>(Role, X, Y);
				N->DefaultValue = Definition.Value.GetVector2(); Parameter = N; break;
			}
			case EMaterialParameterType::Vector:
			{
				auto* N = Add<DMaterialExpressionVector3Parameter>(Role, X, Y);
				N->DefaultValue = Definition.Value.GetVector(); Parameter = N; break;
			}
			case EMaterialParameterType::Vector4:
			{
				auto* N = Add<DMaterialExpressionVector4Parameter>(Role, X, Y);
				N->DefaultValue = Definition.Value.GetVector4(); Parameter = N; break;
			}
			case EMaterialParameterType::Texture:
			{
				auto* N = Add<DMaterialExpressionTextureSampleParameter2D>(Role, X, Y);
				N->DefaultValue = Definition.Value.GetTexture(); N->TextureUsage = Definition.TextureUsage;
				Parameter = N; break;
			}
			}
			require(Parameter);
			Parameter->Metadata = {Definition.Id, Definition.Name, Definition.DisplayName,
				Definition.GroupName, Definition.SortOrder, Definition.Presentation};
			return Parameter;
		}
		static auto Output(FMaterialExpressionSurfaceOutputs& Outputs, uint32 Role) -> FMaterialExpressionInput&
		{
			const std::array Links{&Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic, &Outputs.Roughness,
				&Outputs.AmbientOcclusion, &Outputs.Emissive, &Outputs.Opacity, &Outputs.OpacityMask};
			return *Links.at(Role);
		}
	};
}
