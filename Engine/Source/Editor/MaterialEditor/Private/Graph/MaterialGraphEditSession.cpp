#include "MaterialGraphEditSession.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Graph/MaterialGraphChange.h"
#include "Materials/Material.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include "DObject/Package.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpressionBuild.h"

namespace Durin::MaterialEditor::GraphEditInternals
{
	namespace
	{
		struct FMemberChange
		{
			FPersistentObjectRef Target;
			FProperty* Member = nullptr;
			FName MemberName;
			uint32 ElementIndex = 0;
			FPropertyValueSnapshotPayload Before, After;
			auto Object() const -> DObject* { return Target.Resolve(); }
			auto Property() const -> FProperty*
			{
				auto* Owner = Object();
				return Owner && Owner->GetClass()->FindPropertyByName(MemberName) == Member ? Member : nullptr;
			}
			auto Index() const -> uint32 { return ElementIndex; }
			auto Capture(DObject& Object, FProperty* Property, uint32 Index) -> std::expected<void, FPropertySnapshotError>
			{
				Target = FPersistentObjectRef(&Object); Member = Property; ElementIndex = Index;
				MemberName = Property ? Property->NamePrivate : FName();
				return CapturePropertyValuePayload(this->Property(), &Object, Index, Before);
			}
			auto CaptureAfter() -> std::expected<void, FPropertySnapshotError>
			{ return CapturePropertyValuePayload(Property(), Object(), Index(), After); }
			auto IsNoOp() const -> bool { return Before == After; }
			auto Restore(bool bBefore) const -> FMaterialChangeResult
			{
				if (!Property()) return {{.Code = EMaterialChangeError::MemberUnavailable}};
				const auto Restored = RestorePropertyValuePayload(Property(), Object(), Index(), bBefore ? Before : After);
				if (!Restored) return {{.Code = EMaterialChangeError::PropertyRestore,
					.PropertyCause = std::make_shared<FPropertySnapshotError>(Restored.error())}};
				return {};
			}
		};
		using FNodes = std::vector<TStrongObjectPtr<DMaterialExpression>>;
		auto ReadPresentation(DObject& Owner) -> FMaterialGraphPresentation
		{
			if (auto* M = Cast<DMaterial>(&Owner)) return M->GetMaterialGraphPresentation();
			return {.Nodes = Cast<DMaterialFunction>(&Owner)->GetFunctionPresentation().Nodes};
		}
		auto WritePresentation(DObject& Owner, const FMaterialGraphPresentation& Value) -> void
		{
			if (auto* M = Cast<DMaterial>(&Owner)) M->SetMaterialGraphPresentation(Value);
			else Cast<DMaterialFunction>(&Owner)->SetFunctionPresentation({.Nodes = Value.Nodes});
		}
		template<class TNodes>
		auto SetNodes(DObject& Owner, const TNodes& Nodes) -> void
		{
			auto& Current = FMaterialExpressionEditing::GetExpressions(Owner);
			std::vector<DMaterialExpression*> Previous;
			for (auto& E : Current) Previous.push_back(E.Get());
			Current.clear();
			for (auto& E : Nodes) Current.emplace_back(E.Get());
			FMaterialExpressionEditing::ReconcileOwnership(Owner, Previous);
		}

	}

	struct FGraphEditSession::FImpl
	{
		DObject& Owner;
		FNodes Original;
		std::unordered_set<DMaterialExpression*> OriginalObjects;
		FMaterialGraphPresentation OriginalPresentation;
		FMaterialRootInputEdits Root;
		std::vector<FMemberChange> Members;
		std::unordered_set<DMaterialExpression*> Modified;
		bool bFinished = false;
		std::shared_ptr<const FPropertySnapshotError> CaptureError;
	};
	FGraphEditSession::FGraphEditSession(DObject& Owner)
		: bFunction(Cast<DMaterialFunction>(&Owner) != nullptr),
		Expressions(FMaterialExpressionEditing::GetExpressions(Owner)), Presentation(ReadPresentation(Owner)),
		Impl(std::make_unique<FImpl>(Owner))
	{
		Impl->OriginalPresentation = Presentation;
		for (auto& E : Expressions) { Impl->Original.emplace_back(E.Get()); Impl->OriginalObjects.insert(E.Get()); }
	}
	FGraphEditSession::~FGraphEditSession()
	{
		if (Impl->bFinished) return;
		for (const auto& M : Impl->Members)
			if (!M.Restore(true)) std::terminate();
		SetNodes(Impl->Owner, Impl->Original);
	}
	auto FGraphEditSession::Modify(DMaterialExpression& Expression) -> void
	{
		if (!Impl->Modified.insert(&Expression).second) return;
		// New objects are retained by structural history; they have no prior authored state.
		if (!Impl->OriginalObjects.contains(&Expression)) return;
		Expression.GetClass()->ForEachProperty([&](FProperty* P) {
			if (P->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < P->GetArrayDim(); ++Index)
			{
				FMemberChange M;
				const auto Captured = M.Capture(Expression, P, Index);
				if (Captured) Impl->Members.push_back(std::move(M));
				else if (!Impl->CaptureError) Impl->CaptureError = std::make_shared<FPropertySnapshotError>(Captured.error());
			}
		});
	}
	auto FGraphEditSession::Assign(DMaterialExpression& Target, const DMaterialExpression& Source) -> FMaterialGraphCommandResult
	{
		if (Target.GetClass() != Source.GetClass()) return RejectCommand("Expression assignment requires matching classes.");
		FMaterialGraphCommandResult Result;
		Target.GetClass()->ForEachProperty([&](FProperty* P) {
			if (P->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < P->GetArrayDim(); ++Index)
			{
				if (ArePropertyValuesIdentical(P, &Source, Index, &Target, Index)) continue;
				Modify(Target);
				const auto Copied = P->CopyAssignValue(P->GetValuePtr(&Target, Index), P->GetValuePtr(&Source, Index));
				if (!Copied && Result) Result = RejectCommand("Unable to update the expression properties. " + ToString(Copied.error()));
			}
		});
		return Result;
	}
	auto FGraphEditSession::GetRootInput(EMaterialProperty Property) const -> const FMaterialExpressionInput*
	{
		const auto& Root = Impl->Root;
		if (Property == EMaterialProperty::MP_MaterialAttributes && Root.Attributes) return &*Root.Attributes;
		if (const auto It = Root.Numeric.find(Property); It != Root.Numeric.end()) return &It->second.Connection;
		const auto* Material = Cast<DMaterial>(&Impl->Owner);
		return Material ? Material->GetExpressionInputForProperty(Property) : nullptr;
	}
	auto FGraphEditSession::EditNumericRoot(EMaterialProperty Property) -> FMaterialNumericInput*
	{
		auto& Fields = Impl->Root.Numeric;
		if (auto It = Fields.find(Property); It != Fields.end()) return &It->second;
		const auto Input = Cast<DMaterial>(&Impl->Owner)->GetNumericInputForProperty(Property);
		return Input ? &Fields.emplace(Property, Input->Capture()).first->second : nullptr;
	}
	auto FGraphEditSession::EditRootInput(EMaterialProperty Property) -> FMaterialExpressionInput*
	{
		if (Property == EMaterialProperty::MP_MaterialAttributes)
		{
			auto& Packed = Impl->Root.Attributes;
			if (!Packed) Packed = *GetRootInput(Property);
			return &*Packed;
		}
		auto* Numeric = EditNumericRoot(Property);
		return Numeric ? &Numeric->Connection : nullptr;
	}
	void FGraphEditSession::SetRootMode(bool UseAttributes) { Impl->Root.UseAttributes = UseAttributes; }
	void FGraphEditSession::ResetRoot()
	{
		SetRootMode(false);
		Impl->Root.Attributes.emplace();
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			Impl->Root.Numeric.insert_or_assign(Field.Property, FMaterialNumericInput(uint32(Field.Semantics.Type) + 1));
	}
	auto ValidateGraphEdit(DObject& Owner, const std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>& Links) -> FMaterialGraphCommandResult
	{
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(Owner);
		const auto Validation = FMaterialExpressionEditing::ValidateStorage(Owner);
		if (!Validation) return RejectCommand("The expression storage is invalid.", Validation.Diagnostics);
		std::unordered_set<DMaterialExpression*> Members;
		for (auto& E : Expressions) Members.insert(E.Get());
		bool bMissingSource = false;
		for (auto& E : Expressions) VisitStoredMaterialInputs(*E, [&](const FMaterialExpressionInput& Input) {
			const auto It = Links.find(&Input);
			const auto& Link = It == Links.end() ? Input : It->second;
			bMissingSource |= Link.OutputIndex < 0 || (Link.Expression && !Members.contains(Link.Expression.Get()));
		});
		if (bMissingSource) return RejectCommand("A connection refers to a missing expression.", {{.Message = "Expression input is disconnected or refers to a missing expression."}});
		if (Cast<DMaterialFunction>(&Owner))
		{
			std::vector<DMaterialFunctionInterface*> Roots;
			for (auto& E : Expressions) if (auto* Call = Cast<DMaterialExpressionFunctionCall>(E.Get())) Roots.push_back(Call->Function.Get());
			std::vector<FMaterialFunctionOwnerStamp> Closure;
			if (std::ranges::any_of(Roots, [&](auto* Root) { return Root == &Owner; }))
				return RejectCommand("This edit would introduce recursive function dependencies.");
			const auto Dependencies = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
			if (!Dependencies) return RejectCommand("The function dependencies are invalid.", Dependencies.Diagnostics);
			if (std::ranges::any_of(Closure, [&](const auto& D) { return D.AssetPath == Owner.GetObjectPath(); }))
				return RejectCommand("This edit would introduce recursive function dependencies.");
		}
		return {};
	}
	auto FGraphEditSession::ReplaceExpression(TObjectPtr<DMaterialExpression>& Slot, DMaterialExpression& Replacement) -> void
	{
		auto* Previous = Slot.Get();
		Slot = &Replacement;
		if (!bFunction)
		{
			const auto Rebind = [&](EMaterialProperty Property) {
				const auto* Link = GetRootInput(Property);
				if (Link->Expression.Get() != Previous) return;
				const auto Port = Link->GetOutputId();
				*EditRootInput(Property) = FMaterialExpressionInput(&Replacement, Port.IsValid() ? 0 : Link->OutputIndex, Port);
			};
			Rebind(EMaterialProperty::MP_MaterialAttributes);
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) Rebind(Field.Property);
		}
		for (const auto& Node : Expressions)
			VisitStoredMaterialInputs(*Node, [&](FMaterialExpressionInput& Input) {
				if (Input.Expression.Get() != Previous) return;
				Modify(*Node);
				if (const auto NamedOutput = Input.GetOutputId(); NamedOutput.IsValid())
					Input = FMaterialExpressionInput(&Replacement, 0, NamedOutput);
				else Input.Expression = &Replacement;
			});
	}

	auto FGraphEditSession::IncludeCallOutput(DMaterialExpression* Source, FGuid OutputId) -> void
	{
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Source);
		if (!OutputId.IsValid() || !Call || !Call->Function.IsValid()
			|| std::ranges::contains(Call->FunctionOutputs, OutputId, &FMaterialFunctionOutputBinding::OutputId)) return;
		const auto& Ports = Call->Function->GetFunctionSignature().Outputs;
		if (const auto Port = std::ranges::find(Ports, OutputId, &FMaterialFunctionPort::Id); Port != Ports.end())
		{
			Modify(*Call);
			Call->FunctionOutputs.push_back({Port->Id, Port->Type});
		}
	}

	auto FGraphEditSession::Commit(std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Transactions && ResolveEditorTransactor(ETransactionMode::Record).value_or(nullptr) != Transactions) return RejectCommand("The editor transactor is busy.");
		const auto* Material = Cast<DMaterial>(&Impl->Owner);
		const auto& Root = Impl->Root;
		const bool bRootChanged = Material && ((Root.UseAttributes && *Root.UseAttributes != Material->UsesMaterialAttributes())
			|| (Root.Attributes && *Root.Attributes != *Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes))
			|| std::ranges::any_of(Root.Numeric, [&](const auto& Entry) {
				return *Material->GetNumericInputForProperty(Entry.first) != Entry.second;
			}));
		const bool bGraphWritten = bRootChanged || !Impl->Members.empty() || !std::ranges::equal(Expressions, Impl->Original,
			{}, [](auto& E) { return E.Get(); }, [](auto& E) { return E.Get(); });
		if (bGraphWritten)
		{
			if (auto Valid = ValidateGraphEdit(Impl->Owner); !Valid) return Valid;
			if (!bFunction)
			{
				if (!Root.HasValidWidths(*Material)) return RejectCommand("A material root constant has the wrong width.");
				const auto InBody = [&](const FMaterialExpressionInput& Link) {
					return Link.OutputIndex >= 0 && (!Link.Expression || std::ranges::contains(Expressions, Link.Expression));
				};
				if (!InBody(*GetRootInput(EMaterialProperty::MP_MaterialAttributes)) || !std::ranges::all_of(FMaterialAttributeDefinitionMap::GetDefinitions(), [&](const auto& Field) {
					return InBody(*GetRootInput(Field.Property));
				})) return RejectCommand("A retained material input refers to a missing expression.");
			}
			// Type inference is best effort. Incompatible widths remain compiler diagnostics.
			std::unordered_set<DMaterialExpression*> TypeChanges, ChangedOutputs;
			for (auto& M : Impl->Members)
			{
				if (const auto Captured = M.CaptureAfter(); !Captured)
					return RejectCommand("Unable to inspect the edited expression. " + ToString(Captured.error()));
				if (M.IsNoOp()) continue;
				auto* E = Cast<DMaterialExpression>(M.Object());
				// Constant values never change their class-defined output width.
				// Other fields are conservative seeds: defaults can constrain math widths.
				if (M.Property()->NamePrivate == FName("Value")
					&& (Cast<DMaterialExpressionScalarConstant>(E) || Cast<DMaterialExpressionVector2Constant>(E)
						|| Cast<DMaterialExpressionVector3Constant>(E) || Cast<DMaterialExpressionVector4Constant>(E))) continue;
				TypeChanges.insert(E);
				if (M.Property()->NamePrivate == FName("ResultType")) ChangedOutputs.insert(E);
			}
			for (auto& E : Expressions)
				if (!Impl->OriginalObjects.contains(E.Get())) { TypeChanges.insert(E.Get()); ChangedOutputs.insert(E.Get()); }
			if (!TypeChanges.empty())
			{
				const std::vector<DMaterialExpression*> Seeds(TypeChanges.begin(), TypeChanges.end());
				const std::vector<DMaterialExpression*> Outputs(ChangedOutputs.begin(), ChangedOutputs.end());
				AdaptNumericTypes(*this, Seeds, Outputs);
			}
			if (Impl->CaptureError) return RejectCommand("Unable to record the edited expression. " + ToString(*Impl->CaptureError));
		}
		std::vector<FGuid> PresentationIds;
		for (auto& E : Expressions) PresentationIds.push_back(E->Id);
		Presentation = SanitizeMaterialGraphPresentation(Presentation, PresentationIds);
		const bool bStructural = !std::ranges::equal(Expressions, Impl->Original, {},
			[](auto& E) { return E.Get(); }, [](auto& E) { return E.Get(); });
		std::vector<FMemberChange*> Changed;
		for (auto& Member : Impl->Members)
		{
			if (auto Result = Member.CaptureAfter(); !Result)
				return RejectCommand("Unable to inspect the expression. " + ToString(Result.error()));
			if (!Member.IsNoOp()) Changed.push_back(&Member);
		}
		const bool bPresentation = Presentation != Impl->OriginalPresentation;
		if (!bStructural && Changed.empty() && !bPresentation && !bRootChanged)
			return {.Status = EMaterialGraphCommandStatus::NoChange};

		std::vector<DMaterialExpression*> Removed;
		if (bStructural)
		{
			std::unordered_set<DMaterialExpression*> Current;
			for (const auto& Node : Expressions) Current.insert(Node.Get());
			for (const auto& Old : Impl->Original)
				if (!Current.contains(Old.Get())) Removed.push_back(Old.Get());
		}

		FScopedMaterialGraphChange Batch(Impl->Owner);
		std::vector<TObjectPtr<DMaterialExpression>> Accepted;
		FNodes Added;
		if (Transactions)
		{
			// Rewind the provisional edit before opening the recording.
			if (bStructural)
			{
				for (const auto& Node : Expressions)
					if (!Impl->OriginalObjects.contains(Node.Get())) Added.emplace_back(Node.Get());
				Accepted.swap(Expressions);
				for (const auto& Node : Impl->Original) Expressions.emplace_back(Node.Get());
			}
			for (auto* Member : Changed)
				if (auto Result = Member->Restore(true); !Result) return RejectCommand(FormatMaterialChangeError(Result.Error));
		}
		const bool Nested = Transactions && Transactions->IsActive();
		FScopedTransaction Recording("MaterialGraph", Description, &Impl->Owner, Transactions != nullptr);
		if (Transactions)
		{
			if (!Recording.IsOutstanding()) return RejectCommand("Unable to start the graph transaction.");
			FScopedPackageDirtySuppression Suppress;
			std::unordered_set<DObject*> Recorded;
			auto Record = [&](DObject& Object) {
				if (!Recorded.insert(&Object).second) return true;
				Object.SetFlags(EObjectFlags::Transactional);
				if (Object.Modify(false)) return true;
				if (!Nested) Recording.Cancel();
				return false;
			};
			if ((bStructural || bPresentation || bRootChanged) && !Record(Impl->Owner)) return RejectCommand("Unable to record the graph owner.");
			for (auto* Node : Removed)
				if (!Record(*Node)) return RejectCommand("Unable to record the removed expression.");
			for (auto* Member : Changed)
				if (!Record(*Member->Object())) return RejectCommand("Unable to record the edited expression.");
			for (auto* Member : Changed)
				if (auto Result = Member->Restore(false); !Result)
				{
					for (auto* Original : Changed) if (!Original->Restore(true)) std::terminate();
					if (!Nested) Recording.Cancel();
					return RejectCommand(FormatMaterialChangeError(Result.Error));
				}
			if (bStructural) Expressions.swap(Accepted);
		}
		for (auto* Node : Removed) Node->MarkAsGarbage();
		FMaterialExpressionEditing::ReconcileOwnership(Impl->Owner, {});
		Impl->bFinished = true;
		if (bPresentation) WritePresentation(Impl->Owner, Presentation);
		if (bRootChanged) FMaterialExpressionEditing::ApplyRootEdits(*Cast<DMaterial>(&Impl->Owner), Root);
		if (bRootChanged || bStructural || !Changed.empty()) FMaterialExpressionEditing::Publish(Impl->Owner);
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}
}

namespace Durin::MaterialEditor::GraphEditInternals
{
	auto EditExpressions(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DMaterialExpression* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult
	{
		const bool Nested = Transactions && Transactions->IsActive();
		{
			FScopedTransaction Recording("MaterialGraph", Description, &Owner, Transactions != nullptr);
			if (Transactions)
			{
				if (!Recording.IsOutstanding()) return RejectCommand("Unable to start the graph transaction.");
				FScopedPackageDirtySuppression Suppress;
				for (auto* Object : Objects)
				{
					Object->SetFlags(EObjectFlags::Transactional);
					if (!Object->Modify(false))
					{
						if (!Nested) Recording.Cancel();
						return RejectCommand("Unable to record the expression.");
					}
				}
			}
			Apply();
			Owner.MarkPackageDirty();
		}
		Objects.front()->PostEditChange();
		return {};
	}

}
