#pragma once

#include "AssetForge/Builtins/StandardMaterialFunctions.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"
#include "MaterialExpressionRecipeTestSupport.h"
#include "EngineTestSupport.h"

namespace Durin::Testing
{
	inline auto MakeStandardMaterialExpressionsForTest(const AssetForge::Builtins::FStandardMaterialFunctions& Functions) -> FTestMaterialExpressionGraph
	{
		using Type = EMaterialProgramValueType;
		using Op = EMaterialProgramOpcode;
		using Entry = AssetForge::Builtins::EStandardMaterialFunction;
		InitializeDObjectSystem();
		FTestMaterialExpressionGraph Graph;
		const auto Recipe = MakePBRMaterialParameterDefinitions();
		auto& Presentation = Graph.Presentation;
		Presentation = {};
		const auto Node = [&](Op Opcode, Type ValueType, std::vector<FMaterialExpressionInput> Inputs = {}, FGuid ParameterId = {}) {
			auto& Expression = Graph.Add(Opcode, ValueType, std::move(Inputs), ParameterId, {}, Recipe);
			Expression.Id = {0xf67a24b1, 0x4378491a, 6, static_cast<uint32>(Graph.Expressions.size())};
			return FMaterialExpressionInput{Expression.Id};
		};
		const auto Call = [&](DMaterialFunction* Function, std::vector<FMaterialExpressionFunctionInputBinding> Inputs) {
			TStrongObjectPtr<DMaterialExpressionFunctionCall> Expression(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
			Expression->Id = {0xf67a24b1, 0x4378491a, 6, static_cast<uint32>(Graph.Expressions.size() + 1)};
			Expression->Function = Function; Expression->Inputs = std::move(Inputs);
			for (const auto& Port : Function->GetFunctionSignature().Outputs) Expression->Outputs.push_back({Port.Id, Port.Type});
			const FMaterialExpressionInput Link{Expression->Id, 0, Expression->Outputs.front().OutputId};
			Graph.Expressions.emplace_back(Expression.Get());
			return Link;
		};
		const std::array OutputLinks{&Graph.Outputs.BaseColor, &Graph.Outputs.Normal, &Graph.Outputs.Metallic, &Graph.Outputs.Roughness,
			&Graph.Outputs.AmbientOcclusion, &Graph.Outputs.Emissive, &Graph.Outputs.Opacity, &Graph.Outputs.OpacityMask};
		using ParameterKind = MaterialParameters::EMaterialBuiltinParameterKind;
		constexpr std::array<uint8, 8> Channels{1, 1, 4, 3, 2, 1, 5, 2};
		for (uint32 I = 0; I < 8; ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const auto Parameter = [&](ParameterKind Kind, Type ValueType, int32 Column, int32 Row) {
				const auto Id = GetMaterialSurfaceParameterId(Role, Kind);
				const auto Definition = std::ranges::find(Recipe, Id, &FMaterialParameterDefinition::Id);
				require(Definition != Recipe.end());
				const auto Link = Node(ValueType == Type::Texture2D ? Op::TextureParameter : Op::Parameter, ValueType, {}, Id);
				Presentation.Nodes.push_back({Link.ExpressionId, Column * 320, static_cast<int32>(I) * 600 + Row * 130});
				return Link;
			};
			const auto Factor = Parameter(ParameterKind::Value, GetMaterialSurfaceOutputType(Role), 2, 0);
			const auto Channel = Parameter(ParameterKind::UVChannel, Type::Float, -2, 0);
			const auto Scale = Parameter(ParameterKind::UVScale, Type::Float2, -2, 1);
			const auto Offset = Parameter(ParameterKind::UVOffset, Type::Float2, -2, 2);
			const auto Rotation = Parameter(ParameterKind::UVRotation, Type::Float, -2, 3);
			const auto Coordinates = Node(Op::TextureCoordinates, Type::Float2, {Channel});
			const auto Scaled = Node(Op::Multiply, Type::Float2, {Coordinates, Scale});
			const auto Sine = Node(Op::Sine, Type::Float, {Rotation});
			const auto Cosine = Node(Op::Cosine, Type::Float, {Rotation});
			const auto Swizzle = [&](uint8 Component) {
				const auto Link = Node(Op::Swizzle, Type::Float, {Scaled});
				Cast<DMaterialExpressionSwizzle>(Graph.Expressions.back().Get())->Components = {Component};
				return Link;
			};
			const auto U = Swizzle(0), V = Swizzle(1);
			const auto X = Node(Op::Subtract, Type::Float, {
				Node(Op::Multiply, Type::Float, {Cosine, U}), Node(Op::Multiply, Type::Float, {Sine, V})});
			const auto Y = Node(Op::Add, Type::Float, {
				Node(Op::Multiply, Type::Float, {Sine, U}), Node(Op::Multiply, Type::Float, {Cosine, V})});
			const auto UV = Node(Op::Add, Type::Float2, {Node(Op::MakeFloat2, Type::Float2, {X, Y}), Offset});
			Presentation.Nodes.push_back({UV.ExpressionId, -320, static_cast<int32>(I) * 600});
			const auto TextureId = GetMaterialSurfaceParameterId(Role, ParameterKind::Texture);
			auto Sample = Node(Op::TextureSampleParameter2D, Type::Float4, {UV}, TextureId);
			Presentation.Nodes.push_back({Sample.ExpressionId, 0, static_cast<int32>(I) * 600});
			Sample.OutputIndex = Channels[I];
			if (I == 1)
			{
				Sample = Call(Functions.SampleNormal.Get(), {
					{AssetForge::Builtins::StandardMaterialPortId(Entry::SampleNormal, 1), Type::Texture2D, {Sample.ExpressionId, 7}},
					{AssetForge::Builtins::StandardMaterialPortId(Entry::SampleNormal, 2), Type::Float2, UV}});
				Presentation.Nodes.push_back({Sample.ExpressionId, 320, static_cast<int32>(I) * 600 + 150});
			}
			const auto FirstCompositionNode = Graph.Expressions.size();
			*OutputLinks[I] = Node(I == 1 ? Op::BlendNormalsRNM : I == 5 ? Op::Add : Op::Multiply,
				GetMaterialSurfaceOutputType(Role), {Factor, Sample});
			for (size_t N = FirstCompositionNode; N < Graph.Expressions.size(); ++N)
				Presentation.Nodes.push_back({Graph.Expressions[N]->Id,
					960 + static_cast<int32>((N - FirstCompositionNode) % 3) * 320,
					static_cast<int32>(I) * 600 + static_cast<int32>((N - FirstCompositionNode) / 3) * 170});
		}
		return Graph;
	}

	// Transient ordinary functions, with the same authored graph definitions as shipped assets.
	inline auto SetStandardMaterialExpressionsForTest(DMaterial& Material) -> bool
	{
		using namespace AssetForge::Builtins;
		FStandardMaterialFunctions Functions;
		const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM,
			&Functions.StandardPBR, &Functions.StandardPBR_ORM, &Functions.ImportedSurfaceValues};
		for (uint32 I = 0; I < Slots.size(); ++I)
		{
			auto* Function = NewObject<DMaterialFunction>(&Material, FName(std::format("StandardFunction{}", I + 1)));
			if (!Function || !MakeStandardMaterialFunctionExpressions(
				static_cast<EStandardMaterialFunction>(I + 1), Functions).Apply(*Function)) return false;
			*Slots[I] = Function;
		}
		return static_cast<bool>(MakeStandardMaterialExpressionsForTest(Functions).Apply(Material));
	}
}
