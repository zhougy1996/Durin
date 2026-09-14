#pragma once
#include "MaterialExpressionRecipeTestSupport.h"
#include "EngineTestSupport.h"

// Independent expanded PBR expressions; production assets use the standard recipes.
namespace Durin::Testing
{
	inline auto MakePBRMaterialExpressionsForTest() -> FTestMaterialExpressionGraph
	{
		InitializeDObjectSystem();
		using Role = MaterialParameters::EMaterialBuiltinParameterRole;
		const auto& BaseIds = MaterialParameters::GetBuiltinParameterIds(Role::BaseColor);
		const auto& NormalIds = MaterialParameters::GetBuiltinParameterIds(Role::Normal);
		const auto& MetallicIds = MaterialParameters::GetBuiltinParameterIds(Role::Metallic);
		const auto& RoughnessIds = MaterialParameters::GetBuiltinParameterIds(Role::Roughness);
		const auto& AmbientOcclusionIds = MaterialParameters::GetBuiltinParameterIds(Role::AmbientOcclusion);
		const auto& EmissiveIds = MaterialParameters::GetBuiltinParameterIds(Role::Emissive);
		const auto& OpacityIds = MaterialParameters::GetBuiltinParameterIds(Role::Opacity);
		const auto& OpacityMaskIds = MaterialParameters::GetBuiltinParameterIds(Role::OpacityMask);
		FTestMaterialExpressionGraph Graph;
		std::vector<std::pair<FGuid, FGuid>> UVExpressions;
		Graph.Expressions.reserve(MaterialProgramMaxNodeCount);
		auto AddNode = [&](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType Type,
			std::vector<FMaterialExpressionInput> Inputs = {},
			FGuid ParameterId = {},
			FMaterialProgramLiteral Literal = {})
			-> DMaterialExpression& {
			return Graph.Add(Opcode, Type, std::move(Inputs), ParameterId, Literal);
		};
		auto Parameter = [&](FGuid Id, EMaterialProgramValueType Type)
			-> DMaterialExpression& {
			return AddNode(EMaterialProgramOpcode::Parameter, Type, {}, Id);
		};
		auto Sample = [&](FGuid TextureId) -> DMaterialExpression& {
			auto& Texture = AddNode(
				EMaterialProgramOpcode::TextureParameter,
				EMaterialProgramValueType::Texture2D, {}, TextureId);
			auto& UV = AddNode(
				EMaterialProgramOpcode::Add,
				EMaterialProgramValueType::Float2);
			UVExpressions.emplace_back(UV.Id, TextureId);
			return AddNode(
				EMaterialProgramOpcode::TextureSample2D,
				EMaterialProgramValueType::Float4,
				{MakeLink(Texture), MakeLink(UV)});
		};
		auto Swizzle = [&](DMaterialExpression& Source,
			EMaterialProgramValueType Type,
			std::initializer_list<uint8> Mask) -> DMaterialExpression& {
			auto& Node = AddNode(
				EMaterialProgramOpcode::Swizzle, Type, {MakeLink(Source)});
			Cast<DMaterialExpressionSwizzle>(&Node)->Components.assign(Mask.begin(), Mask.end());
			return Node;
		};
		auto Unary = [&](EMaterialProgramOpcode Opcode,
			DMaterialExpression& Input) -> DMaterialExpression& {
			return AddNode(Opcode, Graph.Types.at(Input.Id), {MakeLink(Input)});
		};
		auto Binary = [&](EMaterialProgramOpcode Opcode,
			DMaterialExpression& A,
			DMaterialExpression& B) -> DMaterialExpression& {
			return AddNode(Opcode, Graph.Types.at(A.Id), {MakeLink(A), MakeLink(B)});
		};
		auto Constant = [&](EMaterialProgramValueType Type,
			float X, float Y = 0.0f, float Z = 0.0f, float W = 0.0f)
			-> DMaterialExpression& {
			return AddNode(EMaterialProgramOpcode::Constant, Type, {}, {},
				{.X = X, .Y = Y, .Z = Z, .W = W});
		};

		auto& BaseParameter = Parameter(
			BaseIds.Value,
			EMaterialProgramValueType::Float3);
		auto& BaseSample = Sample(BaseIds.Texture);
		auto& BaseRgb = Swizzle(
			BaseSample, EMaterialProgramValueType::Float3, {0, 1, 2});
		auto& BaseSaturated = Unary(
			EMaterialProgramOpcode::Saturate, BaseParameter);
		auto& BaseColor = Binary(
			EMaterialProgramOpcode::Multiply, BaseSaturated, BaseRgb);

		auto& NormalParameter = Parameter(
			NormalIds.Value,
			EMaterialProgramValueType::Float3);
		auto& NormalSample = Sample(NormalIds.Texture);
		auto& NormalRg = Swizzle(
			NormalSample, EMaterialProgramValueType::Float2, {0, 1});
		auto& Half = AddNode(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2, {}, {}, {.5f, .5f});
		auto& CenteredNormal = Binary(EMaterialProgramOpcode::Subtract, NormalRg, Half);
		auto& Strength = AddNode(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, {}, {1});
		auto& Strength2 = AddNode(EMaterialProgramOpcode::Splat2, EMaterialProgramValueType::Float2, {MakeLink(Strength)});
		auto& ScaledNormal = Binary(EMaterialProgramOpcode::Multiply, CenteredNormal, Strength2);
		auto& EncodedNormal = Binary(EMaterialProgramOpcode::Add, ScaledNormal, Half);
		auto& DecodedNormal = AddNode(
			EMaterialProgramOpcode::DecodeNormalRG,
			EMaterialProgramValueType::Float3, {MakeLink(EncodedNormal)});
		auto& Normal = Binary(
			EMaterialProgramOpcode::BlendNormalsRNM,
			NormalParameter, DecodedNormal);

		auto MakeScalarProduct = [&](FGuid ParameterId, FGuid TextureId,
			uint8 Component) -> DMaterialExpression& {
			auto& Value = Parameter(
				ParameterId, EMaterialProgramValueType::Float);
			auto& TextureSample = Sample(TextureId);
			auto& Channel = Swizzle(
				TextureSample, EMaterialProgramValueType::Float, {Component});
			auto& SaturatedValue = Unary(
				EMaterialProgramOpcode::Saturate, Value);
			auto& SaturatedChannel = Unary(
				EMaterialProgramOpcode::Saturate, Channel);
			return Binary(
				EMaterialProgramOpcode::Multiply,
				SaturatedValue, SaturatedChannel);
		};

		auto& Metallic = MakeScalarProduct(
			MetallicIds.Value, MetallicIds.Texture, 2);
		auto& RoughnessProduct = MakeScalarProduct(
			RoughnessIds.Value, RoughnessIds.Texture, 1);
		auto& RoughnessMinimum = Constant(
			EMaterialProgramValueType::Float, 0.045f);
		auto& RoughnessMaximum = Constant(
			EMaterialProgramValueType::Float, 1.0f);
		auto& Roughness = AddNode(
			EMaterialProgramOpcode::Clamp,
			EMaterialProgramValueType::Float,
			{MakeLink(RoughnessProduct), MakeLink(RoughnessMinimum),
				MakeLink(RoughnessMaximum)});
		auto& AmbientOcclusion = MakeScalarProduct(
			AmbientOcclusionIds.Value, AmbientOcclusionIds.Texture, 0);

		auto& EmissiveParameter = Parameter(
			EmissiveIds.Value,
			EMaterialProgramValueType::Float3);
		auto& EmissiveSample = Sample(EmissiveIds.Texture);
		auto& EmissiveRgb = Swizzle(
			EmissiveSample, EMaterialProgramValueType::Float3, {0, 1, 2});
		auto& Zero3 = Constant(
			EMaterialProgramValueType::Float3, 0.0f, 0.0f, 0.0f);
		auto& PositiveEmissive = Binary(
			EMaterialProgramOpcode::Maximum, EmissiveParameter, Zero3);
		auto& PositiveEmissiveSample = Binary(
			EMaterialProgramOpcode::Maximum, EmissiveRgb, Zero3);
		auto& Emissive = Binary(
			EMaterialProgramOpcode::Add,
			PositiveEmissive, PositiveEmissiveSample);

		auto& Opacity = MakeScalarProduct(
			OpacityIds.Value, OpacityIds.Texture, 3);
		auto& OpacityMask = MakeScalarProduct(
			OpacityMaskIds.Value, OpacityMaskIds.Texture, 0);

		Graph.Outputs = {
			.BaseColor = MakeLink(BaseColor),
			.Normal = MakeLink(Normal),
			.Metallic = MakeLink(Metallic),
			.Roughness = MakeLink(Roughness),
			.AmbientOcclusion = MakeLink(AmbientOcclusion),
			.Emissive = MakeLink(Emissive),
			.Opacity = MakeLink(Opacity),
			.OpacityMask = MakeLink(OpacityMask)};
		auto AppendNode = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<FMaterialExpressionInput> Inputs = {}, FGuid ParameterId = {}, FMaterialProgramLiteral Literal = {}) -> FMaterialExpressionInput {
			return MakeLink(AddNode(Opcode, Type, std::move(Inputs), ParameterId, Literal));
		};
		auto AppendSwizzleScalar = [&](FMaterialExpressionInput Source, uint8 Component) {
			const auto Link = AppendNode(EMaterialProgramOpcode::Swizzle,
				EMaterialProgramValueType::Float, {Source});
			Cast<DMaterialExpressionSwizzle>(Graph.Expressions.back().Get())->Components = {Component};
			return Link;
		};

		// Complete UV expression slots after assigning the stable surface node identities.
		std::vector<TStrongObjectPtr<DMaterialExpression>> SurfaceNodes = std::move(Graph.Expressions);
		Graph.Expressions.clear();
		Graph.Expressions.reserve(MaterialProgramMaxNodeCount);
		Graph.NextId = static_cast<uint32>(SurfaceNodes.size());
		for (auto& Node : SurfaceNodes)
		{
			const auto UVExpression = std::ranges::find(UVExpressions, Node->Id,
				&std::pair<FGuid, FGuid>::first);
			if (UVExpression == UVExpressions.end())
			{
				Graph.Expressions.push_back(std::move(Node));
				continue;
			}
			const FGuid PreservedId = Node->Id;
			const Role TextureRole = MaterialParameters::FindBuiltinParameterRole(
				UVExpression->second,
				MaterialParameters::EMaterialBuiltinParameterKind::Texture);
			const auto Ids = MaterialParameters::GetBuiltinParameterIds(TextureRole);
			const auto Channel = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float, {}, Ids.UVChannel);
			const auto UV = AppendNode(EMaterialProgramOpcode::UVChannel,
				EMaterialProgramValueType::Float2, {Channel});
			const auto Scale = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float2, {}, Ids.UVScale);
			const auto Scaled = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float2, {UV, Scale});
			const auto Rotation = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float, {}, Ids.UVRotation);
			const auto Sine = AppendNode(EMaterialProgramOpcode::Sine,
				EMaterialProgramValueType::Float, {Rotation});
			const auto Cosine = AppendNode(EMaterialProgramOpcode::Cosine,
				EMaterialProgramValueType::Float, {Rotation});
			const auto X = AppendSwizzleScalar(Scaled, 0);
			const auto Y = AppendSwizzleScalar(Scaled, 1);
			const auto CX = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Cosine, X});
			const auto SY = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Sine, Y});
			const auto SX = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Sine, X});
			const auto CY = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Cosine, Y});
			const auto RotatedX = AppendNode(EMaterialProgramOpcode::Subtract,
				EMaterialProgramValueType::Float, {CX, SY});
			const auto RotatedY = AppendNode(EMaterialProgramOpcode::Add,
				EMaterialProgramValueType::Float, {SX, CY});
			const auto Rotated = AppendNode(EMaterialProgramOpcode::MakeFloat2,
				EMaterialProgramValueType::Float2, {RotatedX, RotatedY});
			const auto Offset = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float2, {}, Ids.UVOffset);
			auto* Final = Cast<DMaterialExpressionAdd>(Node.Get());
			check(Final && Final->Id == PreservedId);
			Final->A = Rotated; Final->B = Offset;
			Graph.Expressions.push_back(std::move(Node));
		}
		return Graph;
	}

}
