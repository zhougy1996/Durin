#pragma once

#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialTestSupport.h"
#include "Materials/MaterialFunction.h"
#include "Editor/EditorTransactionTestSupport.h"

#include "DObject/DefaultObjectGraph.h"
#include "DObject/MathStructs.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialExpressionBuild.h"
#include <set>
#include "Materials/MaterialCookedProgram.h"
#include "StaticMesh/StaticMeshDerivedData.h"

#include <cstring>
#include <limits>
#include <unordered_set>

namespace
{
	auto CaptureMaterialExpressions(const Durin::DMaterial& Material) -> Durin::Testing::FTestMaterialExpressionGraph
	{
		Durin::Testing::FTestMaterialExpressionGraph Graph;
		Graph.Outputs = Material.GetExpressionOutputs();
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			Graph.Expressions.emplace_back(Durin::DuplicateObject(Expression.Get(), nullptr, Durin::NAME_None));
		return Graph;
	}

	auto MakeDefaultMaterialCompilerIR() -> Durin::FMaterialIR
	{
		Durin::FMaterialExpressionBuildContext Empty(std::span<Durin::DMaterialExpression* const>{});
		return Empty.FinishSurface({}).IR;
	}

	auto MakeSyntheticMaterialCompilerInput() -> Durin::FMaterialIRCompilerInput
	{
		InitializeDObjectSystem();
		auto Recipe = Durin::Testing::MakePBRMaterialExpressionsForTest();
		std::vector<Durin::DMaterialExpression*> Expressions;
		for (const auto& Expression : Recipe.Expressions) Expressions.push_back(Expression.Get());
		Durin::FMaterialExpressionBuildContext Context(Expressions);
		auto Built = Context.FinishSurface(Recipe.Outputs);
		check(Built);
		Durin::FMaterialIRCompilerInput Input{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
			.Sources = std::move(Built.Sources)};
		Input.Environment.CompilerIdentity = "slang-test-build;target=spirv;profile=spirv_1_5";
		Input.Environment.Target = "vulkan-spirv-1.5";
		Input.Environment.Dependencies = {
			{"/Engine/MaterialTemplate.slang", {11, 12}},
			{"/Engine/StaticMeshBasePass.slang", {21, 22}}};
		return Input;
	}

	auto ReorderIndependentMaterialIRNodes(Durin::FMaterialIRCompilerInput& Input) -> void
	{
		// Detached IR requires dependencies before consumers. Reverse ready-node
		// priority to exercise equivalent orderings without violating that contract.
		const auto Count = static_cast<uint32>(Input.IR.Nodes.size());
		std::vector<uint32> Pending(Count), Remapping(Count);
		std::vector<std::vector<uint32>> Consumers(Count);
		std::set<uint32> Ready;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			Pending[Index] = static_cast<uint32>(Input.IR.Nodes[Index].Inputs.size());
			for (const auto Dependency : Input.IR.Nodes[Index].Inputs) Consumers.at(Dependency).push_back(Index);
			if (Pending[Index] == 0) Ready.insert(Index);
		}
		std::vector<Durin::FMaterialIRNode> Nodes;
		while (!Ready.empty())
		{
			const auto Index = *Ready.rbegin(); Ready.erase(Index);
			auto Node = Input.IR.Nodes[Index];
			for (auto& Dependency : Node.Inputs) Dependency = Remapping[Dependency];
			Remapping[Index] = static_cast<uint32>(Nodes.size());
			Nodes.push_back(std::move(Node));
			for (const auto Consumer : Consumers[Index]) if (--Pending[Consumer] == 0) Ready.insert(Consumer);
		}
		check(Nodes.size() == Count);
		Input.IR.Nodes = std::move(Nodes);
		for (auto& Root : Input.IR.SurfaceRoot.Inputs)
			if (Root.bExpression) Root.ExpressionIndex = Remapping[Root.ExpressionIndex];
		if (Input.IR.SurfaceRoot.bAggregate)
			Input.IR.SurfaceRoot.AggregateExpressionIndex = Remapping[Input.IR.SurfaceRoot.AggregateExpressionIndex];
		for (auto& Source : Input.Sources) Source.ExpressionIndex = Remapping[Source.ExpressionIndex];
	}

	auto MakeExpandedMaterial(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		if (!FinishMaterialCompileForTest(*Material)) return nullptr;
		return Material;
	}
}
