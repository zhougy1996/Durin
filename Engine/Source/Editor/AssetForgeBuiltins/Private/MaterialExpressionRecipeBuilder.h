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
		auto Parameter(const FMaterialParameterDefinition& Definition, uint32 Role, int32 X, int32 Y, EMaterialProgramValueType OutputType)
			-> DMaterialExpression*
		{
			DMaterialExpressionParameter* Parameter = nullptr;
			switch (Definition.Value.GetType())
			{
			case EMaterialParameterType::Vector:
			case EMaterialParameterType::Vector2:
				// Authored recipes require Vector4 parameter expressions.
				break;
			case EMaterialParameterType::Scalar:
			{
				auto* N = Add<DMaterialExpressionScalarParameter>(Role, X, Y);
				N->DefaultValue = Definition.Value.GetScalar(); N->bHasRange = Definition.bHasRange;
				N->MinimumValue = Definition.MinimumValue; N->MaximumValue = Definition.MaximumValue;
				Parameter = N; break;
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
			if (Definition.Type == EMaterialParameterType::Vector4 && OutputType != EMaterialProgramValueType::Float4)
			{
				auto* Mask = Add<DMaterialExpressionSwizzle>(Role, X + 240, Y);
				Mask->Input = {Parameter->Id}; Mask->Components.clear();
				for (uint8 C = 0; C <= static_cast<uint8>(OutputType); ++C) Mask->Components.push_back(C);
				return Mask;
			}
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
				auto* Rotated = Add<DMaterialExpressionAppendVector>(Role, X += 280, Y);
				Rotated->A = {RX->Id}; Rotated->B = {RY->Id}; UV = {Rotated->Id};
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
