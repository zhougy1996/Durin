#pragma once
#include "FunctionPortTestFixture.h"
#include "Graph/MaterialGraphNodeDisplay.h"
#include "TypedMaterialGraphTestFixture.h"
#include "Graph/MaterialExpressionInputs.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "StandardMaterialFunctionTestFixture.h"
#include "Misc/MountPathTestSupport.h"
#include "MaterialGraphOperations.h"
#include "MaterialGraphDocument.h"
#include "Graph/MaterialGraphEditSession.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "MaterialAssetCreation.h"
#include "Graph/MaterialGraphCanvas.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Widgets/MaterialPreviewFraming.h"

#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "AssetRegistry/Scan.h"
#include "DObject/DefaultObjectGraph.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>
#include <chrono>

#include "NativeDObjectTestSupport.h"


namespace
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Material;

	auto HasGraphDiagnostics(DObject& Owner) -> bool
	{
		const auto* Material = Cast<DMaterial>(&Owner);
		const auto& Collection = Material ? Material->GetExpressionCollection() : Cast<DMaterialFunction>(&Owner)->GetExpressionCollection();
		std::vector<DMaterialExpression*> Nodes;
		for (auto& E : Collection.Expressions) Nodes.push_back(E.Get());
		const auto Result = Material ? MIR::FGraphBuilder::ValidateSurface(Nodes, Material->GetExpressionOutputs())
			: MIR::FGraphBuilder::ValidateFunction(Nodes);
		return !Result && !Result.Diagnostics.empty();
	}


	struct FExpressionGraphSnapshot
	{
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		FMaterialExpressionSurfaceOutputs Outputs;
		FMaterialFunctionSignature Signature;
		auto operator==(const FExpressionGraphSnapshot& Other) const -> bool
		{
			if (Signature != Other.Signature || Outputs != Other.Outputs || Expressions.size() != Other.Expressions.size()) return false;
			for (size_t Index = 0; Index < Expressions.size(); ++Index)
			{
				if (Expressions[Index]->GetClass() != Other.Expressions[Index]->GetClass()) return false;
				bool Identical = true;
				Expressions[Index]->GetClass()->ForEachProperty([&](FProperty* Property) {
					Identical &= ArePropertyValuesIdentical(Property, Expressions[Index].Get(), 0, Other.Expressions[Index].Get(), 0);
				});
				if (!Identical) return false;
			}
			return true;
		}
	};
	auto CaptureExpressions(const DMaterial& Material) -> FExpressionGraphSnapshot
	{
		FExpressionGraphSnapshot Result;
		Result.Outputs = Material.GetExpressionOutputs();
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			Result.Expressions.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None));
		return Result;
	}

	auto CaptureExpressions(const DMaterialFunction& Function) -> FExpressionGraphSnapshot
	{
		FExpressionGraphSnapshot Result;
		Result.Signature = Function.GetFunctionSignature();
		for (const auto& Expression : Function.GetExpressionCollection().Expressions)
			Result.Expressions.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None));
		return Result;
	}

	template<class T, class TOwner> auto FindExpression(const TOwner& Material, FGuid Id) -> const T*
	{
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			if (Expression->Id == Id) return Cast<T>(Expression.Get());
		return nullptr;
	}

	auto FindViewNode(const FMaterialGraphView& View, const FGuid& Id)
		-> const FMaterialGraphNodeView*
	{
		const auto It = std::ranges::find(View.Nodes, Id,
			[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
		return It == View.Nodes.end() ? nullptr : &*It;
	}

	auto ReadVector4Parameter(const DMaterial& Material, FName Name, FVector4& Value) -> bool
	{
		const auto* Definition = Material.FindParameterDefinition(Name);
		FResolvedMaterialParameter Resolved;
		if (!Definition || !Material.ResolveParameterValue(Definition->Id, Resolved)
			|| Resolved.Value.GetType() != EMaterialParameterType::Vector4) return false;
		Value = Resolved.Value.GetVector4();
		return true;
	}

	auto Normalize(const DMaterial& Material) -> MIR::FNormalizationResult
	{
		MIR::FCompilerInput Input;
		FMaterialCompilerEnvironment Environment;
		Environment.CompilerIdentity = "material-graph-operations-test";
		Environment.Target = "vulkan-spirv-1.5";
		if (!SnapshotMaterialCompilerInput(Material, Environment, Input)) return {};
		return MIR::Normalize(Input);
	}

	auto MakeExpandedGraphMaterial(const char* Name) -> DMaterial*
	{
		DMaterial* Material = NewObject<DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)
			|| !FMaterialGraphOperations::Layout(*Material)) return nullptr;
		return Material;
	}
}
