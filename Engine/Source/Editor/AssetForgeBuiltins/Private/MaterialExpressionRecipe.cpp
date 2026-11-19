#include "AssetForge/Builtins/MaterialExpressionRecipe.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
#include "DObject/Class.h"
#include "DObject/Property.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		template<typename A, typename B>
		auto SameExpressions(const A& Left, const B& Right) -> bool
		{
			if (Left.size() != Right.size()) return false;
			for (size_t I = 0; I < Left.size(); ++I)
			{
				auto* L = Left[I].Get();
				auto* R = Right[I].Get();
				if (!L || !R || !HaveSameMaterialExpressionFields(*L, *R)) return false;
			}
			return true;
		}
	}
	auto FMaterialExpressionRecipe::Apply(DMaterial& Material) const -> FMaterialProgramValidationResult
	{
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& Expression : Expressions) Nodes.push_back(Expression.Get());
		auto Result = Material.SetMaterialExpressions(Nodes, Outputs);
		if (Result)
		{
			auto Layout = Presentation;
			if (const auto* Output = Material.GetOutputNode())
				Layout.Nodes.push_back({Output->Id, OutputPosition.first, OutputPosition.second});
			Material.SetMaterialGraphPresentation(std::move(Layout));
		}
		return Result;
	}
	auto FMaterialExpressionRecipe::MatchesGraph(const DMaterial& Material) const -> bool
	{
		std::vector<TObjectPtr<DMaterialExpression>> Body;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			if (!Cast<DMaterialExpressionMaterialOutput>(Expression.Get())) Body.push_back(Expression);
		return CaptureMaterialOutputBindings(Outputs) == CaptureMaterialOutputBindings(Material.GetExpressionOutputs()) && SameExpressions(Expressions, Body);
	}
	auto FMaterialExpressionRecipe::MatchesGraph(const FMaterialExpressionRecipe& Other) const -> bool
	{
		return CaptureMaterialOutputBindings(Outputs) == CaptureMaterialOutputBindings(Other.Outputs) && SameExpressions(Expressions, Other.Expressions);
	}
}
