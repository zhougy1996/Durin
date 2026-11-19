#include "MaterialExpressionOwnership.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionBuild.h"
#include "DObject/Property.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/TransactionObjectReference.h"

#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"

#if DURIN_WITH_EDITORONLY_DATA
namespace Durin
{
	namespace
	{
		// Whole-object replay can restore an Outer. An externally retired expression
		// must never be reattached to a collection that no longer contains it.
		class FExpressionMembership final : public ITransactionObjectAnnotation
		{
		public:
			FExpressionMembership(FPersistentObjectRef InOwner, FPersistentObjectRef InExpression)
				: Owner(std::move(InOwner)), Expression(std::move(InExpression)) {}
			auto Validate() const -> FTransactionCaptureResult override
			{
				auto* Asset = Owner.Resolve();
				const auto* Node = Cast<DMaterialExpression>(Expression.Resolve());
				if (!Asset || !Node || Node->GetOuter() != Asset ||
					!std::ranges::any_of(FMaterialExpressionEditing::GetExpressions(*Asset),
						[&](const auto& Entry) { return Entry.Get() == Node; }))
					return std::unexpected("The graph participants changed outside history.");
				return {};
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				Owner.AddReferencedObjects(Collector);
				Expression.AddReferencedObjects(Collector);
			}
			auto Serialize(FArchive& Archive) -> void override
			{
				for (auto* Ref : {&Owner, &Expression})
				{
					auto Key = Ref->GetKey();
					Key.SerializeForSnapshot(Archive);
					if (Archive.IsLoading() && !Archive.IsError()) *Ref = FPersistentObjectRef::FromKey(Key);
				}
			}
			auto GetAllocatedSize() const -> size_t override { return sizeof(*this) + Owner.GetAllocatedSize() + Expression.GetAllocatedSize(); }
			auto CreateReplacement(const FObjectReplacementMap& Map) const
				-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string> override
			{
				auto Asset = Owner.CreateReplacement(Map);
				if (!Asset) return std::unexpected(Asset.error());
				auto Node = Expression.CreateReplacement(Map);
				if (!Node) return std::unexpected(Node.error());
				return std::make_shared<FExpressionMembership>(std::move(*Asset), std::move(*Node));
			}
		private:
			FPersistentObjectRef Owner;
			FPersistentObjectRef Expression;
		};
	}

	auto DMaterialExpression::FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		auto* Asset = GetOuter();
		if (!Cast<DMaterial>(Asset) && !Cast<DMaterialFunction>(Asset)) return {};
		return std::make_shared<FExpressionMembership>(FPersistentObjectRef(Asset),
			FPersistentObjectRef::FromKey(FObjectKey(this)));
	}

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
		if (Expressions.size() > MaterialProgramMaxNodeCount) return Fail("Material graph exceeds the node limit.");
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
#endif

namespace Durin::Private
{
	auto ValidateExpressionOwnership(const DObject& Owner,
		const FMaterialExpressionCollection& Collection) -> std::expected<void, std::string>
	{
		std::unordered_set<const DObject*> Owned;
		for (const auto& Expression : Collection.Expressions)
			if (!IsValid(Expression.Get()) || Expression->GetOuter() != &Owner || !Owned.insert(Expression.Get()).second)
			{
				return std::unexpected("Expression collection contains a missing, shared, or wrongly owned child.");
			}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				return std::unexpected("Owner contains an abandoned expression child outside its collection.");
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
			auto* Copy = Duplicated.value();
			if (!Copy)
			{
				FMaterialProgramValidationResult Result;
				Result.Diagnostics.push_back({.Message = ToString(Duplicated.error())});
				return Result;
			}
			Copies.Expressions.emplace_back(Copy);
			const auto SortRoot = [](FMaterialAttributeRoot& Root) {
				std::ranges::sort(Root.Properties.Bindings, {}, &FMaterialAttributeBinding::AttributeID);
			};
			if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(Copy)) SortRoot(Output->Outputs);
			if (auto* Make = Cast<DMaterialExpressionMakeMaterialAttributes>(Copy)) SortRoot(Make->Values);
			if (auto* Get = Cast<DMaterialExpressionGetMaterialAttributes>(Copy)) std::ranges::sort(Get->AttributeIDs);
			if (auto* Set = Cast<DMaterialExpressionSetMaterialAttributes>(Copy))
				std::ranges::sort(Set->Attributes, {}, &FMaterialExpressionAttributeBinding::AttributeID);
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Copy))
				std::ranges::sort(Input->Port.Default.Surface.Bindings, {}, &FMaterialAttributeValue::AttributeID);
		}
		TStrongObjectPtr<DObject> Retired(NewObject<DObject>(nullptr, "RetiredMaterialExpressions"));
		for (auto& Expression : Collection.Expressions) if (Expression) Expression->SetOuterPrivate(Retired.Get());
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(&Owner);
		Collection = std::move(Copies);
		return {.bSucceeded = true};
	}
}
