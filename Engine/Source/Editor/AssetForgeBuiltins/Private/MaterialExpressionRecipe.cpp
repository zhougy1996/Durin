#include "AssetForge/Builtins/MaterialExpressionRecipe.h"
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
				const auto* L = Left[I].Get(); const auto* R = Right[I].Get();
				if (!L || !R || L->GetClass() != R->GetClass()) return false;
				bool bSame = true;
				L->GetClass()->ForEachProperty([&](FProperty* Property) {
					for (uint32 Element = 0; bSame && Element < Property->GetArrayDim(); ++Element)
						bSame = ComparePropertyValues(Property, L, Element, R, Element) == EPropertyIdentityResult::Identical;
				});
				if (!bSame) return false;
			}
			return true;
		}
	}
	auto FMaterialExpressionRecipe::Apply(DMaterial& Material) const -> FMaterialProgramValidationResult
	{
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& Expression : Expressions) Nodes.push_back(Expression.Get());
		auto Result = Material.SetMaterialExpressions(Nodes, Outputs);
		if (Result) Material.SetMaterialGraphPresentation(Presentation);
		return Result;
	}
	auto FMaterialExpressionRecipe::MatchesGraph(const DMaterial& Material) const -> bool
	{
		return Outputs == Material.GetExpressionOutputs() && SameExpressions(Expressions, Material.GetExpressionCollection().Expressions);
	}
	auto FMaterialExpressionRecipe::MatchesGraph(const FMaterialExpressionRecipe& Other) const -> bool
	{
		return Outputs == Other.Outputs && SameExpressions(Expressions, Other.Expressions);
	}
}
