#include "MaterialExpressionOwnership.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionBuild.h"
#include "DObject/Property.h"
#include "DObject/TransactionObjectEvent.h"

#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"

#if DURIN_WITH_EDITORONLY_DATA
namespace Durin
{
	// Replay has loaded every participant before native completion. Repair child
	// ownership here so observers and package export see the same collection.
	static auto RestoreExpressionOwnership(DObject& Owner) -> void
	{
		std::vector<DMaterialExpression*> Children;
		for (auto* Object : GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly))
			if (auto* Expression = Cast<DMaterialExpression>(Object)) Children.push_back(Expression);
		FMaterialExpressionEditing::ReconcileOwnership(Owner, Children);
	}

	auto DMaterial::PostEditUndo() -> void { RestoreExpressionOwnership(*this); }
	auto DMaterialFunction::PostEditUndo() -> void { RestoreExpressionOwnership(*this); }

	auto DMaterial::PostTransacted(const FTransactionObjectEvent& Event) -> void
	{
		if (Event.Type != ETransactionObjectEventType::UndoRedo) return;
		if (std::ranges::any_of(Event.Change.ChangedProperties, [](FName Name) {
			static const std::array RootProperties{
				FName("ExpressionCollection"), FName("MaterialAttributes"), FName("bUseMaterialAttributes"),
				FName("BaseColor"), FName("Normal"), FName("Metallic"), FName("Roughness"),
				FName("AmbientOcclusion"), FName("EmissiveColor"), FName("Opacity"), FName("OpacityMask"),
				FName("WorldPositionOffset"), FName("Specular"), FName("ClearCoat"), FName("ClearCoatRoughness"), FName("Refraction")};
			return std::ranges::contains(RootProperties, Name);
		}))
			FMaterialExpressionEditing::Publish(*this, Event.OperationId);
		else if (std::ranges::any_of(Event.Change.ChangedProperties, [](FName Name) { return Name == "GraphPresentation" || Name == "EditorX" || Name == "EditorY"; }))
			GraphChanges.PublishPresentation(*this);
	}

	auto DMaterialFunction::PostTransacted(const FTransactionObjectEvent& Event) -> void
	{
		if (Event.Type != ETransactionObjectEventType::UndoRedo) return;
		if (std::ranges::contains(Event.Change.ChangedProperties, FName("ExpressionCollection")))
			FMaterialExpressionEditing::Publish(*this, Event.OperationId);
		else if (std::ranges::contains(Event.Change.ChangedProperties, FName("Presentation")))
			GraphChanges.PublishPresentation(*this);
	}

	auto DMaterialExpression::PostTransacted(const FTransactionObjectEvent& Event) -> void
	{
		if (Event.Type != ETransactionObjectEventType::UndoRedo || IsGarbage()) return;
		auto* Owner = GetOuter();
		if (Cast<DMaterial>(Owner) || Cast<DMaterialFunction>(Owner)) FMaterialExpressionEditing::Publish(*Owner, Event.OperationId);
	}

	auto FMaterialExpressionEditing::GetExpressions(DObject& Owner) -> std::vector<TObjectPtr<DMaterialExpression>>&
	{
		check(IsInGameThread());
		if (auto* Material = Cast<DMaterial>(&Owner)) return Material->ExpressionCollection.Expressions;
		auto* Function = Cast<DMaterialFunction>(&Owner);
		check(Function);
		return Function->ExpressionCollection.Expressions;
	}

	auto FMaterialExpressionEditing::SetRootInputs(DMaterial& Owner, FMaterialRootInputs Inputs) -> bool
	{
		if (Inputs.FindInvalidConstantWidth()) return false;
		Owner.ApplyRootInputs(std::move(Inputs));
		return true;
	}

	void FMaterialExpressionEditing::VisitRootConnections(DMaterial& Owner, const std::function<void(FMaterialExpressionInput&)>& Visit)
	{
		Visit(Owner.MaterialAttributes);
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			Visit(const_cast<FMaterialExpressionInput&>(*Owner.GetExpressionInputForProperty(Field.Property)));
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
			if (GetMaterialDomainInputProperties(Material->Domain).empty()) return Fail("Unsupported material domain.");
			std::vector<FMaterialParameterDefinition> Schema;
			return DMaterial::DeriveExpressionParameterSchema(Material->ExpressionCollection, Schema);
		}
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& E : Expressions)
		{
			if (Cast<DMaterialExpressionParameter>(E.Get()))
				return Fail("Functions cannot own root parameters.");
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

	auto FMaterialExpressionEditing::Publish(DObject& Owner, uint64 OperationId) -> void
	{
		// Native replay reports every changed expression. Recompute the settled
		// owner's signature, revisions and previews once for this operation.
		if (OperationId)
		{
			auto* Material = Cast<DMaterial>(&Owner);
			auto& Published = Material ? Material->LastGraphReplayOperation : Cast<DMaterialFunction>(&Owner)->LastGraphReplayOperation;
			if (std::exchange(Published, OperationId) == OperationId) return;
		}
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
		for (const auto& Node : Collection.Expressions)
		{
			bool Valid = true;
			VisitStoredMaterialInputs(*Node, [&](const FMaterialExpressionInput& Input) {
				if (Input.OutputIndex < 0 || (Input.Expression && !Owned.contains(Input.Expression.Get())))
					Valid = false;
			});
			if (!Valid) return std::unexpected("Expression input has a negative output index or refers outside its graph.");
		}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				return std::unexpected("Owner contains an abandoned expression child outside its collection.");
			}
		return {};
	}

	auto ReplaceOwnedExpressions(DObject& Owner, FMaterialExpressionCollection& Collection,
		std::span<DMaterialExpression* const> Expressions, FMaterialRootInputs* Root) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "MaterialExpressionApply"));
		FMaterialExpressionCollection Copies;
		Copies.Expressions.reserve(Expressions.size());
		for (auto* Expression : Expressions)
		{
			const auto Duplicated = DuplicateObject(Expression, Staging.Get(), FName(std::string("Expression_") + Expression->Id.ToString()));
			if (!Duplicated)
			{
				FMaterialProgramValidationResult Result;
				Result.Diagnostics.push_back({.Message = ToString(Duplicated.error())});
				return Result;
			}
			auto* Copy = *Duplicated;
			Copies.Expressions.emplace_back(Copy);
			if (auto* Get = Cast<DMaterialExpressionGetMaterialAttributes>(Copy)) std::ranges::sort(Get->AttributeIDs);
			if (auto* Set = Cast<DMaterialExpressionSetMaterialAttributes>(Copy))
				std::ranges::sort(Set->Attributes, {}, &FMaterialExpressionAttributeBinding::AttributeID);
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Copy))
				std::ranges::sort(Input->Port.Default.Surface.Bindings, {}, &FMaterialAttributeValue::AttributeID);
		}
		std::unordered_map<const DMaterialExpression*, DMaterialExpression*> Remap;
		for (size_t Index = 0; Index < Expressions.size(); ++Index) Remap.emplace(Expressions[Index], Copies.Expressions[Index].Get());
		bool Valid = true;
		const auto RemapInput = [&](FMaterialExpressionInput& Input) {
			if (!Input.Expression) return;
			const auto Source = Remap.find(Input.Expression.Get());
			if (Source == Remap.end()) Valid = false;
			else Input.Expression = Source->second;
		};
		for (const auto& Copy : Copies.Expressions) VisitStoredMaterialInputs(*Copy, RemapInput);
		if (Root)
		{
			RemapInput(Root->Attributes);
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
				RemapInput(Root->Get(Field.Property).Connection);
		}
		if (!Valid) return {.Diagnostics = {{.Message = "Connection refers outside the candidate graph."}}};
		// Retire the old identities for history and remove them from the package
		// hierarchy before a prepared graph can be published.
		for (auto& Expression : Collection.Expressions)
			if (Expression)
			{
				Expression->MarkAsGarbage();
				Expression->SetOuterPrivate(nullptr);
			}
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(&Owner);
		Collection = std::move(Copies);
		return {.bSucceeded = true};
	}
}
