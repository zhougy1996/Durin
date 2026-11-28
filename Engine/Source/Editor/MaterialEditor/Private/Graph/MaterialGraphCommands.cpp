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

	auto GetExpressionOutputPin(const DMaterialExpression& Expression, int32 Ordinal) -> std::optional<FExpressionOutputPin>
	{
		if (const auto Output = Expression.GetOutput(Ordinal))
			return FExpressionOutputPin{*Output, Output->StoredIndex.value_or(Ordinal)};
		return {};
	}

	auto FindExpressionOutputPin(const DMaterialExpression& Expression, int32 SourceIndex, FGuid PortId) -> std::optional<FExpressionOutputPin>
	{
		if (SourceIndex < 0 && !PortId.IsValid()) return {};
		for (int32 Ordinal = 0; const auto Pin = GetExpressionOutputPin(Expression, Ordinal); ++Ordinal)
			if (PortId.IsValid() ? Pin->Output.PortId == PortId : Pin->SourceIndex == SourceIndex) return Pin;
		return {};
	}

	auto GetExpressionInputFallback(const DMaterialExpression& Expression, int32 Index) -> std::vector<float>
	{
		const auto Opcode = Expression.GetCreationInfo().Opcode;
		if (!Opcode || Index < 0) return {};
		const auto Type = IsMaterialSamplingNode(*Opcode)
			? Expression.GetInputValueType(Index) : Expression.GetOutputValueType(0);
		return GetMaterialNumericInputFallback(*Opcode, Type.value_or(EMaterialProgramValueType::Float), static_cast<uint32>(Index));
	}

	auto ResolveGraphOutput(DMaterialExpression* Source, int32 Index, FGuid Port, FProposedCallOutputs& CallOutputs) -> FMaterialExpressionInput
	{
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Source);
		if (!Call)
		{
			FMaterialExpressionInput Link{Source, Index, Port};
			if (Source && !FindExpressionOutputPin(*Source, Link.OutputIndex)) Link.OutputIndex = -1;
			return Link;
		}
		const auto Pin = FindExpressionOutputPin(*Call, Index, Port);
		if (!Pin) return {Source, -1};
		if (!Port.IsValid()) return {Source, Pin->SourceIndex};
		auto Proposed = CallOutputs.find(Call);
		const auto& Outputs = Proposed == CallOutputs.end() ? Call->FunctionOutputs : Proposed->second;
		const auto Bound = std::ranges::find(Outputs, Port, &FMaterialFunctionOutputBinding::OutputId);
		if (Bound != Outputs.end()) return {Source, static_cast<int32>(Bound - Outputs.begin())};
		auto& Candidate = CallOutputs.try_emplace(Call, Call->FunctionOutputs).first->second;
		Candidate.push_back({Pin->Output.PortId, Pin->Output.Type});
		return {Source, static_cast<int32>(Candidate.size() - 1)};
	}

	auto CollectCallOutputEdits(FProposedCallOutputs& Outputs, std::vector<DObject*>& Objects) -> void
	{
		std::erase_if(Outputs, [](const auto& Entry) { return Entry.first->FunctionOutputs == Entry.second; });
		for (const auto& [Call, Bindings] : Outputs) Objects.push_back(Call);
	}

	auto WriteCallOutputs(FProposedCallOutputs& Outputs) -> void
	{
		for (auto& [Call, Bindings] : Outputs) Call->FunctionOutputs.swap(Bindings);
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

	auto RecordGraphObjects(DObject& Owner, DTransactor* Transactions, std::string_view Description,
		std::span<DObject* const> Objects, bool bBatchGraphChanges,
		const std::function<FMaterialGraphCommandResult()>& Apply) -> FMaterialGraphCommandResult
	{
		if (Transactions && ResolveEditorTransactor(true).value_or(nullptr) != Transactions)
			return RejectCommand("The editor transactor is busy.");
		if (std::ranges::any_of(Objects, [](const auto* Object) { return !IsValid(Object); }))
			return RejectCommand("An edited graph object is unavailable.");
		const bool bNested = Transactions && Transactions->IsActive();
		std::optional<FScopedMaterialGraphChange> Batch;
		if (bBatchGraphChanges) Batch.emplace(Owner);
		FScopedTransaction Recording("MaterialGraph", Description, &Owner, Transactions && !bNested);
		if (Transactions)
		{
			if (!bNested && !Recording.IsOutstanding()) return RejectCommand("Unable to start the graph transaction.");
			std::unordered_set<DObject*> Recorded;
			for (auto* Object : Objects)
			{
				if (!Recorded.insert(Object).second) continue;
				Object->SetFlags(EObjectFlags::Transactional);
				Object->Modify(false);
			}
		}
		return Apply();
	}

	auto RecordGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DObject* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult
	{
		return RecordGraphObjects(Owner, Transactions, Description, Objects, /*bBatchGraphChanges=*/true, [&]() -> FMaterialGraphCommandResult {
			Apply();
			FMaterialExpressionEditing::Publish(Owner);
			return {};
		});
	}

	auto EditExpressions(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DMaterialExpression* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult
	{
		const std::vector<DObject*> Participants(Objects.begin(), Objects.end());
		return RecordGraphEdit(Owner, Transactions, std::move(Description), Participants, Apply);
	}
}
