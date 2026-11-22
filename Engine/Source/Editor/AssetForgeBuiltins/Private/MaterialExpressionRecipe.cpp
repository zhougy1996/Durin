#include "AssetForge/Builtins/MaterialExpressionRecipe.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "DObject/Class.h"
#include "DObject/Property.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto SameRoot(const FMaterialRootInputs& Recipe, const auto& Other) -> bool
		{
			const auto SameLink = [](const FMaterialExpressionInput& A, const FMaterialExpressionInput& B) {
				return A.GetExpressionId() == B.GetExpressionId() && A.OutputIndex == B.OutputIndex;
			};
			if (Recipe.bUseMaterialAttributes != Other.UsesMaterialAttributes()
				|| !SameLink(Recipe.Attributes, *Other.GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes))) return false;
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			{
				const auto& A = Recipe.Get(Field.Property);
				const auto B = Other.GetNumericInputForProperty(Field.Property);
				if (!SameLink(A.Connection, B->Connection) || A.UseConstant != B->UseConstant
					|| !std::ranges::equal(A.Constant, B->Constant)) return false;
			}
			return true;
		}

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
			Material.SetRootPosition(OutputPosition.first, OutputPosition.second);
			Material.SetMaterialGraphPresentation(Presentation);
		}
		return Result;
	}
	auto FMaterialExpressionRecipe::MatchesGraph(const DMaterial& Material) const -> bool
	{
		return SameRoot(Outputs, Material)
			&& SameExpressions(Expressions, Material.GetExpressionCollection().Expressions);
	}
	auto FMaterialExpressionRecipe::MatchesGraph(const FMaterialExpressionRecipe& Other) const -> bool
	{
		return SameRoot(Outputs, Other.Outputs) && SameExpressions(Expressions, Other.Expressions);
	}
}
