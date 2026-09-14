#pragma once

#include "AssetForge/Builtins/StandardMaterialFunctions.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"

namespace Durin::Testing
{
	// Builds a comprehensive parameter/function graph entirely inside test fixtures.
	struct FStandardMaterialTestBuilder
	{
		FMaterialFunctionGraph Graph;
		auto Node(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<FMaterialProgramLink> Inputs = {}) -> FMaterialProgramLink
		{
			const FGuid Id{0xf67a24b1, 0x4378491a, 6, static_cast<uint32>(Graph.Nodes.size() + 1)};
			Graph.Nodes.push_back({.Id = Id, .Opcode = Opcode, .ResultType = Type, .Inputs = std::move(Inputs)});
			return {.SourceNodeId = Id};
		}
		auto Call(DMaterialFunction* Function, std::vector<FMaterialFunctionInputBinding> Inputs) -> FMaterialProgramLink
		{
			const auto Link = Node(EMaterialProgramOpcode::FunctionCall, Function->GetFunctionSignature().Outputs.front().Type);
			FMaterialFunctionCall Call{.NodeId = Link.SourceNodeId, .Function = Function, .Inputs = std::move(Inputs)};
			for (const auto& Output : Function->GetFunctionSignature().Outputs)
				Call.Outputs.push_back({Output.Id, Output.Type});
			Graph.Calls.push_back(std::move(Call));
			return {.SourceNodeId = Link.SourceNodeId, .SourceOutputId = Graph.Calls.back().Outputs.front().OutputId};
		}
	};

	inline auto MakeStandardMaterialProgramForTest(const AssetForge::Builtins::FStandardMaterialFunctions& Functions,
		std::vector<FMaterialFunctionCall>& OutCalls, FMaterialGraphPresentation& OutPresentation) -> FMaterialProgram
	{
		using Type = EMaterialProgramValueType;
		using Op = EMaterialProgramOpcode;
		using Entry = AssetForge::Builtins::EStandardMaterialFunction;
		FStandardMaterialTestBuilder B;
		const auto Recipe = MakePBRMaterialParameterDefinitions();
		FMaterialGraphPresentation Presentation{.bHasMaterialOutputPosition = true, .MaterialOutputX = 2000, .MaterialOutputY = 400};
		FMaterialProgram Result;
		using ParameterKind = MaterialParameters::EMaterialBuiltinParameterKind;
		constexpr std::array<uint8, 8> Channels{1, 6, 4, 3, 2, 1, 5, 2};
		for (uint32 I = 0; I < 8; ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const auto Parameter = [&](ParameterKind Kind, Type ValueType, int32 Column, int32 Row) {
				const auto Id = GetMaterialSurfaceParameterId(Role, Kind);
				const auto Definition = std::ranges::find(Recipe, Id, &FMaterialParameterDefinition::Id);
				require(Definition != Recipe.end());
				const auto Link = B.Node(ValueType == Type::Texture2D ? Op::TextureParameter : Op::Parameter, ValueType);
				B.Graph.Nodes.back().Parameter = *Definition;
				Presentation.Nodes.push_back({Link.SourceNodeId, Column * 320, static_cast<int32>(I) * 600 + Row * 130});
				return Link;
			};
			const auto Factor = Parameter(ParameterKind::Value, GetMaterialSurfaceOutputType(Role), 2, 0);
			const auto Channel = Parameter(ParameterKind::UVChannel, Type::Float, -2, 0);
			const auto Scale = Parameter(ParameterKind::UVScale, Type::Float2, -2, 1);
			const auto Offset = Parameter(ParameterKind::UVOffset, Type::Float2, -2, 2);
			const auto Rotation = Parameter(ParameterKind::UVRotation, Type::Float, -2, 3);
			const auto UV = B.Node(Op::TextureCoordinates, Type::Float2, {Channel, Scale, Offset, Rotation});
			Presentation.Nodes.push_back({UV.SourceNodeId, -320, static_cast<int32>(I) * 600});
			auto Sample = B.Node(Op::TextureSampleParameter2D, Type::Float4, {UV});
			const auto TextureId = GetMaterialSurfaceParameterId(Role, ParameterKind::Texture);
			B.Graph.Nodes.back().Parameter = *std::ranges::find(Recipe, TextureId, &FMaterialParameterDefinition::Id);
			Presentation.Nodes.push_back({Sample.SourceNodeId, 0, static_cast<int32>(I) * 600});
			Sample.SourceOutputIndex = Channels[I];
			if (I == 1)
			{
				Sample = B.Call(Functions.DecodeImportedNormalRG.Get(), {{AssetForge::Builtins::StandardMaterialPortId(Entry::DecodeImportedNormalRG, 1), Type::Float2, Sample}});
				Presentation.Nodes.push_back({Sample.SourceNodeId, 320, static_cast<int32>(I) * 600 + 150});
			}
			const auto FirstCompositionNode = B.Graph.Nodes.size();
			GetMaterialSurfaceOutputLink(Result.Outputs, Role) = B.Node(I == 1 ? Op::BlendNormalsRNM : I == 5 ? Op::Add : Op::Multiply,
				GetMaterialSurfaceOutputType(Role), {Factor, Sample});
			for (size_t N = FirstCompositionNode; N < B.Graph.Nodes.size(); ++N)
				Presentation.Nodes.push_back({B.Graph.Nodes[N].Id,
					960 + static_cast<int32>((N - FirstCompositionNode) % 3) * 320,
					static_cast<int32>(I) * 600 + static_cast<int32>((N - FirstCompositionNode) / 3) * 170});
		}
		Result.Nodes = std::move(B.Graph.Nodes);
		OutCalls = std::move(B.Graph.Calls);
		OutPresentation = std::move(Presentation);
		return Result;
	}

	// Transient ordinary functions, with the same authored graph definitions as shipped assets.
	inline auto SetStandardMaterialProgramForTest(DMaterial& Material) -> bool
	{
		using namespace AssetForge::Builtins;
		FStandardMaterialFunctions Functions;
		const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM,
			&Functions.StandardPBR, &Functions.StandardPBR_ORM, &Functions.ImportedSurfaceValues, &Functions.DecodeImportedNormalRG};
		for (uint32 I = 0; I < Slots.size(); ++I)
		{
			auto* Function = NewObject<DMaterialFunction>(&Material, FName(std::format("StandardFunction{}", I + 1)));
			if (!Function || !Function->SetFunctionGraph(MakeStandardMaterialFunctionGraph(
				static_cast<EStandardMaterialFunction>(I + 1), Functions))) return false;
			*Slots[I] = Function;
		}
		std::vector<FMaterialFunctionCall> Calls;
		FMaterialGraphPresentation Presentation;
		auto Program = MakeStandardMaterialProgramForTest(Functions, Calls, Presentation);
		if (!Material.SetMaterialProgramAndFunctionCalls(std::move(Program), std::move(Calls))) return false;
		return Material.SetMaterialGraphPresentation(std::move(Presentation));
	}
}
