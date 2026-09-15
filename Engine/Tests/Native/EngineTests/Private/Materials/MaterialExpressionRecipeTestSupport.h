#pragma once
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Class.h"
#include <functional>
#include <unordered_map>

namespace Durin::Testing
{
	constexpr auto MakeCanonicalNodeId(uint32 Index) -> FGuid
	{ return {0x4d350001u, 0x7a6b4c21u, 0x91d2e3f4u, Index + 1u}; }
	inline auto MakeLink(const DMaterialExpression& Expression) -> FMaterialExpressionInput
	{ return {Expression.Id}; }
	template<class Visitor> auto VisitTestExpressionInputs(DMaterialExpression& Expression, Visitor&& Visit) -> void
	{
		Expression.GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
				&& static_cast<FStructProperty*>(Property)->GetStruct() == FMaterialExpressionInput::StaticStruct())
				Visit(*static_cast<FMaterialExpressionInput*>(Property->GetValuePtr(&Expression)));
		});
	}
	struct FTestMaterialExpressionGraph
	{
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		FMaterialExpressionSurfaceOutputs Outputs;
		FMaterialGraphPresentation Presentation;
		uint32 NextId = 0;
		std::unordered_map<FGuid, EMaterialProgramValueType> Types;
		auto Apply(DMaterial& Material) const -> FMaterialProgramValidationResult
		{
			std::vector<DMaterialExpression*> Values;
			for (const auto& Expression : Expressions) Values.push_back(Expression.Get());
			auto Result = Material.SetMaterialExpressions(Values, Outputs);
			if (Result && !Presentation.Nodes.empty()) Material.SetMaterialGraphPresentation(Presentation);
			return Result;
		}
		auto SetParameterDefaults(std::span<const FMaterialParameterDefinition> Definitions) -> void
		{
			for (auto& Expression : Expressions)
			{
				const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get());
				if (!Parameter) continue;
				const auto Definition = std::ranges::find(Definitions, Parameter->Metadata.Id, &FMaterialParameterDefinition::Id);
				check(Definition != Definitions.end());
				check(Definition->Value.GetType() == Parameter->GetParameterDefinition().Type);
				if (auto* E = Cast<DMaterialExpressionScalarParameter>(Expression.Get())) E->DefaultValue = Definition->Value.GetScalar();
				else if (auto* E = Cast<DMaterialExpressionVector2Parameter>(Expression.Get())) E->DefaultValue = Definition->Value.GetVector2();
				else if (auto* E = Cast<DMaterialExpressionVector3Parameter>(Expression.Get())) E->DefaultValue = Definition->Value.GetVector();
				else if (auto* E = Cast<DMaterialExpressionVector4Parameter>(Expression.Get())) E->DefaultValue = Definition->Value.GetVector4();
				else if (auto* E = Cast<DMaterialExpressionTextureParameter>(Expression.Get()))
				{
					const auto& Value = Definition->Value.GetTexture();
					E->DefaultValue = {Value.Texture, Value.SamplerState, Value.TextureFallback};
				}
			}
		}
		auto Add(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<FMaterialExpressionInput> Inputs, FGuid ParameterId, FMaterialProgramLiteral Literal,
			std::span<const FMaterialParameterDefinition> Definitions = GetPBRMaterialParameterDefinitions()) -> DMaterialExpression&
		{
			DMaterialExpression* Expression = nullptr;
			if (Opcode == EMaterialProgramOpcode::Parameter || Opcode == EMaterialProgramOpcode::TextureParameter
				|| Opcode == EMaterialProgramOpcode::TextureSampleParameter2D)
			{
				const auto Definition = std::ranges::find(Definitions, ParameterId, &FMaterialParameterDefinition::Id);
				check(Definition != Definitions.end());
				switch (Definition->Value.GetType())
				{
				case EMaterialParameterType::Scalar:
					{ auto* E = NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None); E->DefaultValue = Definition->Value.GetScalar();
					E->bHasRange = Definition->bHasRange; E->MinimumValue = Definition->MinimumValue; E->MaximumValue = Definition->MaximumValue; Expression = E; break; }
				case EMaterialParameterType::Vector2:
					{ auto* E = NewObject<DMaterialExpressionVector2Parameter>(nullptr, NAME_None); E->DefaultValue = Definition->Value.GetVector2(); Expression = E; break; }
				case EMaterialParameterType::Vector:
					{ auto* E = NewObject<DMaterialExpressionVector3Parameter>(nullptr, NAME_None); E->DefaultValue = Definition->Value.GetVector(); Expression = E; break; }
				case EMaterialParameterType::Vector4:
					{ auto* E = NewObject<DMaterialExpressionVector4Parameter>(nullptr, NAME_None); E->DefaultValue = Definition->Value.GetVector4(); Expression = E; break; }
				case EMaterialParameterType::Texture:
					{ DMaterialExpressionTextureParameter* E = Opcode == EMaterialProgramOpcode::TextureSampleParameter2D
						? NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None) : NewObject<DMaterialExpressionTextureParameter>(nullptr, NAME_None); const auto& V = Definition->Value.GetTexture();
					E->DefaultValue = {V.Texture, V.SamplerState, V.TextureFallback}; E->TextureUsage = Definition->TextureUsage; Expression = E; break; }
				}
				Cast<DMaterialExpressionParameter>(Expression)->Metadata = {Definition->Id, Definition->Name, Definition->DisplayName,
					Definition->GroupName, Definition->SortOrder, Definition->Presentation};
			}
			else if (Opcode == EMaterialProgramOpcode::Constant)
			{
				if (Type == EMaterialProgramValueType::Float) { auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); E->Value = Literal.X; Expression = E; }
				if (Type == EMaterialProgramValueType::Float2) { auto* E = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); E->Value = {Literal.X, Literal.Y}; Expression = E; }
				if (Type == EMaterialProgramValueType::Float3) { auto* E = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None); E->Value = {Literal.X, Literal.Y, Literal.Z}; Expression = E; }
				if (Type == EMaterialProgramValueType::Float4) { auto* E = NewObject<DMaterialExpressionVector4Constant>(nullptr, NAME_None); E->Value = {Literal.X, Literal.Y, Literal.Z, Literal.W}; Expression = E; }
			}
			else switch (Opcode)
			{
			case EMaterialProgramOpcode::Add: Expression = NewObject<DMaterialExpressionAdd>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::TextureCoordinates: Expression = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Lerp: Expression = NewObject<DMaterialExpressionLerp>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Multiply: Expression = NewObject<DMaterialExpressionMultiply>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Maximum: Expression = NewObject<DMaterialExpressionMaximum>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Saturate: Expression = NewObject<DMaterialExpressionSaturate>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Clamp: Expression = NewObject<DMaterialExpressionClamp>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Sine: Expression = NewObject<DMaterialExpressionSine>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Cosine: Expression = NewObject<DMaterialExpressionCosine>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::TextureSample2D: Expression = NewObject<DMaterialExpressionTextureSample2D>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::UVChannel: Expression = NewObject<DMaterialExpressionUVChannel>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Splat3: Expression = NewObject<DMaterialExpressionSplat3>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Subtract: Expression = NewObject<DMaterialExpressionSubtract>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::MakeSurface: Expression = NewObject<DMaterialExpressionMakeSurface>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::Swizzle:
			{
				auto* Mask = NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None);
				Mask->Components.clear();
				for (uint8 Channel = 0; Channel <= static_cast<uint8>(Type); ++Channel) Mask->Components.push_back(Channel);
				Expression = Mask;
				break;
			}
			case EMaterialProgramOpcode::Splat2: Expression = NewObject<DMaterialExpressionSplat2>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::MakeFloat2: Expression = NewObject<DMaterialExpressionMakeVector2>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::DecodeNormalRG: Expression = NewObject<DMaterialExpressionDecodeNormalRG>(nullptr, NAME_None); break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Expression = NewObject<DMaterialExpressionBlendNormalsRNM>(nullptr, NAME_None); break;
			default: check(false); break;
			}
			check(Expression);
			Expression->Id = MakeCanonicalNodeId(NextId++);
			if (auto* Width = Expression->GetClass()->FindPropertyByName("ResultType"))
				*static_cast<EMaterialProgramValueType*>(Width->GetValuePtr(Expression)) = Type;
			uint32 Index = 0;
			VisitTestExpressionInputs(*Expression, [&](FMaterialExpressionInput& Input) {
				if (Index < Inputs.size()) Input = Inputs[Index];
				++Index;
			});
			Types.emplace(Expression->Id, Type);
			Expressions.emplace_back(Expression);
			return *Expression;
		}
	};
}
