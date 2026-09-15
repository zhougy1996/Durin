#include "MaterialExpressionOwnership.h"

#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin::Private
{
	auto ValidateExpressionOwnership(const DObject& Owner,
		const FMaterialExpressionCollection& Collection, std::string& OutError) -> bool
	{
		std::unordered_set<const DObject*> Owned;
		for (const auto& Expression : Collection.Expressions)
			if (!IsValid(Expression.Get()) || Expression->GetOuter() != &Owner || !Owned.insert(Expression.Get()).second)
			{
				OutError = "Expression collection contains a missing, shared, or wrongly owned child.";
				return false;
			}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				OutError = "Owner contains an abandoned expression child outside its collection.";
				return false;
			}
		return true;
	}

	auto ReplaceOwnedExpressions(DObject& Owner, FMaterialExpressionCollection& Collection,
		std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "MaterialExpressionApply"));
		FMaterialExpressionCollection Copies;
		Copies.Expressions.reserve(Expressions.size());
		for (auto* Expression : Expressions)
		{
			auto* Copy = DuplicateObject(Expression, Staging.Get(), FName(std::string("Expression_") + Expression->Id.ToString()));
			if (!Copy)
			{
				FMaterialProgramValidationResult Result;
				Result.Diagnostics.push_back({.Message = "Unable to duplicate the expression candidate."});
				return Result;
			}
			Copies.Expressions.emplace_back(Copy);
		}
		TStrongObjectPtr<DObject> Retired(NewObject<DObject>(nullptr, "RetiredMaterialExpressions"));
		for (auto& Expression : Collection.Expressions) if (Expression) Expression->SetOuterPrivate(Retired.Get());
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(&Owner);
		Collection = std::move(Copies);
		return {.bSucceeded = true};
	}
}
