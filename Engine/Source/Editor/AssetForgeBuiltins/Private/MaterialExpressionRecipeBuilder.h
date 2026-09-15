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
		// Preserve imported/template UV parameters as ordinary, shareable graph operations.
		auto TransformUV(uint32 Role, int32 X, int32 Y, FMaterialExpressionInput Channel,
			FMaterialExpressionInput Scale, FMaterialExpressionInput Offset, FMaterialExpressionInput Rotation)
			-> FMaterialExpressionInput
		{
			using Type = EMaterialProgramValueType;
			auto* Coordinates = Add<DMaterialExpressionTextureCoordinates>(Role, X, Y);
			Coordinates->Channel = Channel;
			FMaterialExpressionInput UV{Coordinates->Id};
			if (Scale.ExpressionId.IsValid())
			{
				auto* N = Add<DMaterialExpressionMultiply>(Role, X += 280, Y);
				N->ResultType = Type::Float2; N->A = UV; N->B = Scale; UV = {N->Id};
			}
			if (Rotation.ExpressionId.IsValid())
			{
				auto* Sine = Add<DMaterialExpressionSine>(Role, X, Y + 180);
				auto* Cosine = Add<DMaterialExpressionCosine>(Role, X, Y + 360);
				Sine->ResultType = Cosine->ResultType = Type::Float;
				Sine->Input = Cosine->Input = Rotation;
				auto* U = Add<DMaterialExpressionSwizzle>(Role, X += 280, Y);
				auto* V = Add<DMaterialExpressionSwizzle>(Role, X, Y + 180);
				U->Input = V->Input = UV; U->Components = {0}; V->Components = {1};
				const auto Product = [&](FGuid A, FGuid B, int32 Row) {
					auto* N = Add<DMaterialExpressionMultiply>(Role, X + 280, Y + Row);
					N->ResultType = Type::Float; N->A = {A}; N->B = {B}; return FMaterialExpressionInput{N->Id};
				};
				const auto CU = Product(Cosine->Id, U->Id, 0), SV = Product(Sine->Id, V->Id, 180);
				const auto SU = Product(Sine->Id, U->Id, 360), CV = Product(Cosine->Id, V->Id, 540);
				auto* RX = Add<DMaterialExpressionSubtract>(Role, X += 560, Y);
				auto* RY = Add<DMaterialExpressionAdd>(Role, X, Y + 180);
				RX->ResultType = RY->ResultType = Type::Float;
				RX->A = CU; RX->B = SV; RY->A = SU; RY->B = CV;
				auto* Rotated = Add<DMaterialExpressionMakeVector2>(Role, X += 280, Y);
				Rotated->X = {RX->Id}; Rotated->Y = {RY->Id}; UV = {Rotated->Id};
			}
			if (Offset.ExpressionId.IsValid())
			{
				auto* N = Add<DMaterialExpressionAdd>(Role, X + 280, Y);
				N->ResultType = Type::Float2; N->A = UV; N->B = Offset; UV = {N->Id};
			}
			return UV;
		}
		static auto Output(FMaterialExpressionSurfaceOutputs& Outputs, uint32 Role) -> FMaterialExpressionInput&
		{
			const std::array Links{&Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic, &Outputs.Roughness,
				&Outputs.AmbientOcclusion, &Outputs.Emissive, &Outputs.Opacity, &Outputs.OpacityMask};
			return *Links.at(Role);
		}
	};
}
