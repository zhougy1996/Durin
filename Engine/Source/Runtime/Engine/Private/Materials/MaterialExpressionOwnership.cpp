#include "MaterialExpressionOwnership.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionBuild.h"
#include "DObject/Property.h"

#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin
{
	auto FMaterialExpressionEditing::GetExpressions(DObject& Owner) -> std::vector<TObjectPtr<DMaterialExpression>>&
	{
		check(IsInGameThread());
		if (auto* Material = Cast<DMaterial>(&Owner)) return Material->ExpressionCollection.Expressions;
		auto* Function = Cast<DMaterialFunction>(&Owner);
		check(Function);
		return Function->ExpressionCollection.Expressions;
	}

	auto FMaterialExpressionEditing::ValidateStorage(DObject& Owner) -> FMaterialProgramValidationResult
	{
		const auto& Expressions = GetExpressions(Owner);
		const auto Fail = [](std::string Message) -> FMaterialProgramValidationResult {
			return {.Diagnostics = {{.Message = std::move(Message)}}};
		};
		if (Expressions.size() > MaterialProgramMaxNodeCount) return Fail("The graph exceeds the node limit.");
		std::unordered_set<FGuid> Ids;
		for (const auto& E : Expressions)
		{
			if (!IsValid(E.Get()) || !E->Id.IsValid() || !Ids.insert(E->Id).second)
				return Fail("Expression identities must be valid and unique.");
			if (E->GetOuter() && E->GetOuter() != &Owner
				&& (Cast<DMaterial>(E->GetOuter()) || Cast<DMaterialFunction>(E->GetOuter())))
				return Fail("Expression children cannot be shared between graph owners.");
		}
		if (auto* Material = Cast<DMaterial>(&Owner))
		{
			if (std::ranges::count_if(Expressions, [](const auto& E) { return Cast<DMaterialExpressionMaterialOutput>(E.Get()) != nullptr; }) != 1)
				return Fail("A material graph requires exactly one output node.");
			if (GetMaterialDomainOutputPins(Material->Domain).empty()) return Fail("Unsupported material domain.");
			std::vector<FMaterialParameterDefinition> Schema;
			return DMaterial::DeriveExpressionParameterSchema(Material->ExpressionCollection, Schema);
		}
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& E : Expressions)
		{
			if (Cast<DMaterialExpressionParameter>(E.Get()) || Cast<DMaterialExpressionMaterialOutput>(E.Get()))
				return Fail("Functions cannot own root parameters or material outputs.");
			Nodes.push_back(E.Get());
		}
		return ValidateMaterialFunctionSignature(DeriveMaterialFunctionSignature(Nodes));
	}

	auto FMaterialExpressionEditing::ReconcileOwnership(DObject& Owner,
		std::span<DMaterialExpression* const> Previous) -> void
	{
		auto& Expressions = GetExpressions(Owner);
		std::unordered_set<DMaterialExpression*> Current;
		for (auto& E : Expressions) Current.insert(E.Get());
		for (auto* E : Previous) if (!Current.contains(E) && E->GetOuter() == &Owner) E->SetOuterPrivate(nullptr);
		for (auto& E : Expressions)
			if (E->GetOuter() != &Owner)
			{
				// Detached drafts may share a temporary name. Package exports require
				// a unique logical identity under their final owner.
				E->Rename(FName(std::string("Expression_") + E->Id.ToString()));
				E->SetOuterPrivate(&Owner);
			}
	}

	auto FMaterialExpressionEditing::Publish(DObject& Owner) -> void
	{
		FPropertyChangedEvent Event;
		Event.MemberProperty = Owner.GetClass()->FindPropertyByName("ExpressionCollection");
		Owner.MarkPackageDirty();
		Owner.PostEditChangeProperty(Event);
	}
}

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
