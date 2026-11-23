#include "MaterialGraphCommands.h"

#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialExpressionInputs.h"

namespace Durin::MaterialEditor::GraphEditInternals
{
	auto SetNodeLayout(DMaterialExpression& Expression, int32 X, int32 Y, std::string Label) -> void
	{
		if (X < -MaterialGraphPresentationCoordinateLimit || X > MaterialGraphPresentationCoordinateLimit
			|| Y < -MaterialGraphPresentationCoordinateLimit || Y > MaterialGraphPresentationCoordinateLimit) return;
		Expression.EditorLayout = {true, X, Y, std::move(Label)};
	}

	auto ResolveGraphOutput(DMaterialExpression* Source, int32 Index, FGuid Port, FProposedCallOutputs& CallOutputs) -> FMaterialExpressionInput
	{
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Source);
		if (!Call || !Port.IsValid()) return {Source, Index, Port};
		auto Proposed = CallOutputs.find(Call);
		const auto& Outputs = Proposed == CallOutputs.end() ? Call->FunctionOutputs : Proposed->second;
		const auto Bound = std::ranges::find(Outputs, Port, &FMaterialFunctionOutputBinding::OutputId);
		if (Bound != Outputs.end()) return {Source, static_cast<int32>(Bound - Outputs.begin())};
		if (!Call->Function.IsValid()) return {Source, -1};
		const auto& Ports = Call->Function->GetFunctionSignature().Outputs;
		const auto Found = std::ranges::find(Ports, Port, &FMaterialFunctionPort::Id);
		if (Found == Ports.end()) return {Source, -1};
		auto& Candidate = CallOutputs.try_emplace(Call, Call->FunctionOutputs).first->second;
		Candidate.push_back({Found->Id, Found->Type});
		return {Source, static_cast<int32>(Candidate.size() - 1)};
	}

	auto PrepareExpressionReplacement(FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root,
		FProposedLinks& Links, DMaterialExpression& Previous, DMaterialExpression& Replacement) -> void
	{
		auto& Nodes = Collection.Expressions;
		const auto Slot = std::ranges::find(Nodes, &Previous, [](const auto& Node) { return Node.Get(); });
		check(Slot != Nodes.end());
		*Slot = &Replacement;
		Replacement.EditorLayout = Previous.EditorLayout;
		const auto Rebind = [&](const FMaterialExpressionInput& Input) {
			const auto Port = Input.GetOutputId();
			return FMaterialExpressionInput(&Replacement, Port.IsValid() ? 0 : Input.OutputIndex, Port);
		};
		if (Root)
		{
			const auto RebindRoot = [&](EMaterialProperty Property) {
				auto* Input = Root->GetExpressionInputForProperty(Property);
				if (Input->Expression.Get() == &Previous) *Input = Rebind(*Input);
			};
			RebindRoot(EMaterialProperty::MP_MaterialAttributes);
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) RebindRoot(Field.Property);
		}
		for (const auto& Node : Nodes)
			VisitStoredMaterialInputs(*Node, [&](FMaterialExpressionInput& Input) {
				if (Input.Expression.Get() == &Previous) Links.emplace(&Input, Rebind(Input));
			});
	}

	auto ValidateProposedGraph(DObject& Owner, const FMaterialExpressionCollection& Collection,
		const FProposedLinks& Links, const FMaterialRootInputs* Root,
		const FMaterialParameterDefinition* SharedParameter) -> FMaterialGraphCommandResult
	{
		const auto Validation = FMaterialExpressionEditing::ValidateStorage(Owner, Collection, SharedParameter);
		if (!Validation) return RejectCommand("The expression storage is invalid.", Validation.Diagnostics);
		std::unordered_set<DMaterialExpression*> Members;
		for (const auto& Node : Collection.Expressions) Members.insert(Node.Get());
		const auto InBody = [&](const FMaterialExpressionInput& Input) {
			return Input.OutputIndex >= 0 && (!Input.Expression || Members.contains(Input.Expression.Get()));
		};
		bool bMissingSource = false;
		for (const auto& Node : Collection.Expressions)
			VisitStoredMaterialInputs(*Node, [&](const FMaterialExpressionInput& Input) {
				const auto Proposed = Links.find(&Input);
				bMissingSource |= !InBody(Proposed == Links.end() ? Input : Proposed->second);
			});
		if (bMissingSource) return RejectCommand("A connection refers to a missing expression.",
			{{.Message = "Expression input is disconnected or refers to a missing expression."}});
		if (Root)
		{
			if (Root->FindInvalidConstantWidth()) return RejectCommand("A material root constant has the wrong width.");
			if (!InBody(Root->Attributes) || !std::ranges::all_of(FMaterialAttributeDefinitionMap::GetDefinitions(),
				[&](const auto& Field) { return InBody(Root->Get(Field.Property).Connection); }))
				return RejectCommand("A retained material input refers to a missing expression.");
		}
		if (Cast<DMaterialFunction>(&Owner))
		{
			std::vector<DMaterialFunctionInterface*> Roots;
			for (const auto& Node : Collection.Expressions)
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Node.Get())) Roots.push_back(Call->Function.Get());
			if (std::ranges::any_of(Roots, [&](auto* Root) { return Root == &Owner; }))
				return RejectCommand("This edit would introduce recursive function dependencies.");
			std::vector<FMaterialFunctionOwnerStamp> Closure;
			const auto Dependencies = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
			if (!Dependencies) return RejectCommand("The function dependencies are invalid.", Dependencies.Diagnostics);
			if (std::ranges::any_of(Closure, [&](const auto& Entry) { return Entry.AssetPath == Owner.GetObjectPath(); }))
				return RejectCommand("This edit would introduce recursive function dependencies.");
		}
		return {};
	}

	auto ValidateGraphEdit(DObject& Owner, const FProposedLinks& Links) -> FMaterialGraphCommandResult
	{
		const auto* Material = Cast<DMaterial>(&Owner);
		const auto& Collection = Material ? Material->GetExpressionCollection() : Cast<DMaterialFunction>(&Owner)->GetExpressionCollection();
		return ValidateProposedGraph(Owner, Collection, Links, nullptr, nullptr);
	}

	auto RecordGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DObject* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult
	{
		if (Transactions && ResolveEditorTransactor(ETransactionMode::Record).value_or(nullptr) != Transactions)
			return RejectCommand("The editor transactor is busy.");
		if (std::ranges::any_of(Objects, [](const auto* Object) { return !IsValid(Object); }))
			return RejectCommand("An edited graph object is unavailable.");
		const bool bNested = Transactions && Transactions->IsActive();
		FScopedMaterialGraphChange Batch(Owner);
		FScopedTransaction Recording("MaterialGraph", Description, &Owner, Transactions != nullptr);
		if (Transactions)
		{
			if (!Recording.IsOutstanding()) return RejectCommand("Unable to start the graph transaction.");
			FScopedPackageDirtySuppression Suppress;
			std::unordered_set<DObject*> Recorded;
			for (auto* Object : Objects)
			{
				if (!Recorded.insert(Object).second) continue;
				Object->SetFlags(EObjectFlags::Transactional);
				if (Object->Modify(false)) continue;
				if (!bNested) Recording.Cancel();
				return RejectCommand("Unable to record the expression.");
			}
		}
		Apply();
		FMaterialExpressionEditing::Publish(Owner);
		return {};
	}

	auto EditExpressions(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DMaterialExpression* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult
	{
		const std::vector<DObject*> Participants(Objects.begin(), Objects.end());
		return RecordGraphEdit(Owner, Transactions, std::move(Description), Participants, Apply);
	}
}
