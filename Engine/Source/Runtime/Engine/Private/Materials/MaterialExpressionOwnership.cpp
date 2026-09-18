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
		const auto Fail = [](FMaterialError Error) -> FMaterialProgramValidationResult {
			return {.Diagnostics = {{.Error = std::move(Error)}}};
		};
		if (Expressions.size() > MaterialProgramMaxNodeCount) return Fail(EMaterialExpressionError::GraphExceedsNodeLimit);
		std::unordered_set<FGuid> Ids;
		for (const auto& E : Expressions)
		{
			if (!IsValid(E.Get()) || !E->Id.IsValid() || !Ids.insert(E->Id).second)
				return Fail(EMaterialExpressionError::IdentitiesValidUnique);
			if (E->GetOuter() && E->GetOuter() != &Owner
				&& (Cast<DMaterial>(E->GetOuter()) || Cast<DMaterialFunction>(E->GetOuter())))
				return Fail(EMaterialExpressionError::ChildrenSharedBetweenGraphOwners);
		}
		if (auto* Material = Cast<DMaterial>(&Owner))
		{
			if (std::ranges::count_if(Expressions, [](const auto& E) { return Cast<DMaterialExpressionMaterialOutput>(E.Get()) != nullptr; }) != 1)
				return Fail(EMaterialExpressionError::InvalidOutputCount);
			if (GetMaterialDomainOutputPins(Material->Domain).empty()) return Fail(EMaterialExpressionError::UnsupportedMaterialDomain);
			std::vector<FMaterialParameterDefinition> Schema;
			return DMaterial::DeriveExpressionParameterSchema(Material->ExpressionCollection, Schema);
		}
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& E : Expressions)
		{
			if (Cast<DMaterialExpressionParameter>(E.Get()) || Cast<DMaterialExpressionMaterialOutput>(E.Get()))
				return Fail(EMaterialExpressionError::FunctionsOwnRootParametersMaterialOutputs);
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
		const FMaterialExpressionCollection& Collection) -> FMaterialOperationResult
	{
		std::unordered_set<const DObject*> Owned;
		for (const auto& Expression : Collection.Expressions)
			if (!IsValid(Expression.Get()) || Expression->GetOuter() != &Owner || !Owned.insert(Expression.Get()).second)
			{
				return {EMaterialExpressionError::CollectionContainsMissingSharedWronglyOwnedChild};
			}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				return {EMaterialExpressionError::OwnerContainsAbandonedExpressionChildOutsideCollection};
			}
		return {};
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
			const auto Duplicated = DuplicateObject(Expression, Staging.Get(), FName(std::string("Expression_") + Expression->Id.ToString()));
			auto* Copy = Duplicated.Object;
			if (!Copy)
			{
				FMaterialProgramValidationResult Result;
				FMaterialError Error(EMaterialExpressionError::UnableDuplicateExpressionCandidate);
				Error.DuplicationCause = std::make_shared<FObjectGraphError>(Duplicated.Error);
				Result.Diagnostics.push_back({.Error = std::move(Error)});
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
