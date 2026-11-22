#include "MaterialGraph.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "MaterialGraphPinSource.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/Material.h"
#include "DObject/ObjectLifecycle.h"
#include "Graph/MaterialGraphEditSession.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpressionTextureSample.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto IsOutput(const FMaterialGraphPinAddress& Pin) -> bool
		{
			return Pin.Kind == EMaterialGraphPinKind::Output || Pin.Kind == EMaterialGraphPinKind::FunctionOutput;
		}
		auto ResolveExpression(DObject* Owner, const FGuid& Id) -> DMaterialExpression*
		{
			const auto* Material = Cast<DMaterial>(Owner);
			const auto* Function = Cast<DMaterialFunction>(Owner);
			if (!Material && !Function) return nullptr;
			const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions : Function->GetExpressionCollection().Expressions;
			for (const auto& Expression : Expressions)
				if (Expression.IsValid() && Expression->Id == Id) return Expression.Get();
			return nullptr;
		}
	}

	namespace GraphEditInternals
	{
		// Update inferred widths on live objects, recording each participant before
		// writing. Unresolvable types remain compiler diagnostics during editing.
		template<class TWrite>
		auto InferNumericTypes(std::span<const TObjectPtr<DMaterialExpression>> Nodes, uint32& InferredCount,
			std::span<DMaterialExpression* const> ChangedNodes, std::span<DMaterialExpression* const> ChangedOutputNodes, TWrite Write,
			const std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>& Links = {},
			const std::unordered_map<const FMaterialNumericInput*, FMaterialNumericInput>& Numerics = {},
			std::map<DMaterialExpression*, EMaterialProgramValueType> Resolved = {}) -> bool
		{
			using Type = EMaterialProgramValueType;
			const auto Catalog = EnumerateCatalog();
			const auto Effective = [&](const FMaterialExpressionInput& Input) -> const FMaterialExpressionInput& {
				const auto It = Links.find(&Input);
				return It == Links.end() ? Input : It->second;
			};
			const auto EffectiveNumeric = [&](const FMaterialNumericInput& Input) -> const FMaterialNumericInput& {
				const auto It = Numerics.find(&Input);
				return It == Numerics.end() ? Input : It->second;
			};
			std::unordered_set<DMaterialExpression*> Expressions;
			std::unordered_set<DMaterialExpression*> Active;
			for (const auto& E : Nodes) Expressions.insert(E.Get());
			// Build reverse edges once for this edit. Only the downstream closure is
			// eligible for inference; unrelated objects retain their authored state.
			std::unordered_map<DMaterialExpression*, std::vector<DMaterialExpression*>> Consumers;
			for (const auto& E : Nodes)
				VisitStoredMaterialInputs(*E, [&](const FMaterialExpressionInput& Input) {
					if (Effective(Input).IsConnected()) Consumers[Effective(Input).Expression.Get()].push_back(E.Get());
				});
			std::unordered_set<DMaterialExpression*> Seeds(ChangedNodes.begin(), ChangedNodes.end()), Affected = Seeds, ChangedTypes;
			for (const auto& [Expression, Width] : Resolved) ChangedTypes.insert(Expression);
			const std::unordered_set<DMaterialExpression*> ForcedOutputs(ChangedOutputNodes.begin(), ChangedOutputNodes.end());
			std::vector<DMaterialExpression*> Pending(ChangedNodes.begin(), ChangedNodes.end());
			for (size_t I = 0; I < Pending.size(); ++I)
				for (const auto& Consumer : Consumers[Pending[I]])
					if (Affected.insert(Consumer).second) Pending.push_back(Consumer);
			bool bValid = true;
			std::function<Type(const FMaterialExpressionInput&)> Resolve;
			Resolve = [&](const FMaterialExpressionInput& Input) -> Type {
				auto* E = Input.Expression.Get();
				if (!Expressions.contains(E)) { bValid = false; return Type::Float; }
				if (Seeds.contains(E) && !Resolved.contains(E)) ChangedTypes.insert(E);
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(E))
				{
					if (Input.OutputIndex >= 0 && static_cast<size_t>(Input.OutputIndex) < Call->FunctionOutputs.size())
						return Call->FunctionOutputs[Input.OutputIndex].ExpectedType;
					bValid = false; return Type::Float;
				}
				if (const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(E))
					return Terminal->Port.Type;
				const auto Shape = std::ranges::find(Catalog, E->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Shape == Catalog.end()) { bValid = false; return Type::Float; }
				const auto Opcode = Shape->Opcode;
				if (IsMaterialSamplingNode(Opcode))
				{
					if (const auto* Output = FindMaterialSampleOutput(Opcode, Input.OutputIndex)) return Output->Type;
					bValid = false; return Type::Float;
				}
				if (Opcode == EMaterialProgramOpcode::GetMaterialAttributes)
				{
					const auto* Definition = std::in_range<uint8>(Input.OutputIndex)
						? FMaterialAttributeDefinitionMap::Find(static_cast<EMaterialProperty>(Input.OutputIndex)) : nullptr;
					if (Definition) return Definition->Semantics.Type;
					bValid = false; return Type::Float;
				}
				if (Resolved.contains(E)) return Resolved.at(E);
				if (!Active.insert(E).second) { bValid = false; return Type::Float; }
				auto* Property = E->GetClass()->FindPropertyByName("ResultType");
				auto Result = Property ? *static_cast<Type*>(Property->GetValuePtr(E)) : Shape->ResultType;
				if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(E))
					Result = static_cast<Type>(Swizzle->Components.size() - 1);
				const auto PreviousType = Result;
				if (!Affected.contains(E)) { Active.erase(E); return Result; }
				bool bInputChanged = false;
				VisitMaterialExpressionInputs(*E, [&](uint32, const FMaterialExpressionInput& Operand) {
					if (!Effective(Operand).IsConnected()) return;
					Resolve(Effective(Operand));
					bInputChanged |= ChangedTypes.contains(Effective(Operand).Expression.Get());
				});
				if (!Seeds.contains(E) && !bInputChanged)
				{
					Active.erase(E); Resolved.emplace(E, Result); return Result;
				}
				if (IsMaterialAdaptiveNumeric(Opcode) || Opcode == EMaterialProgramOpcode::AppendVector) ++InferredCount;
				if (auto* Append = Cast<DMaterialExpressionAppendVector>(E))
				{
					const auto Width = [&](const FMaterialNumericInput& Numeric) -> size_t {
						const auto& Operand = Effective(Numeric.Connection);
						const auto& Default = EffectiveNumeric(Numeric);
						if (!Operand.IsConnected()) return Default.UseConstant ? Default.Constant.size() : 1;
						const auto Value = Resolve(Operand);
						return Value <= Type::Float4 ? static_cast<size_t>(Value) + 1 : 5;
					};
					const auto AWidth = Width(Append->A);
					const auto BWidth = Width(Append->B);
					if (!AWidth || !BWidth || AWidth + BWidth > 4) bValid = false;
					else
					{
						const auto Inferred = static_cast<Type>(AWidth + BWidth - 1);
						if (Append->ResultType != Inferred) { Write(*Append, Append->ResultType, Inferred); }
						Result = Inferred;
					}
				}
				if (IsMaterialAdaptiveNumeric(Opcode))
				{
					Type Inferred = Type::Float;
					bool bOperand = false;
					const auto Merge = [&](Type Value) {
						bOperand = true;
						if (Value > Type::Float4 || (Value != Type::Float && Inferred != Type::Float && Value != Inferred)) bValid = false;
						if (Value != Type::Float) Inferred = Value;
					};
					VisitMaterialExpressionInputs(*E, [&](uint32 Slot, FMaterialExpressionInput& Operand) {
						if (Opcode == EMaterialProgramOpcode::Lerp && Slot == 2) return;
						if (Effective(Operand).IsConnected()) Merge(Resolve(Effective(Operand)));
						else if (const auto* Numeric = FindMaterialNumericInput(*E, Operand); Numeric && EffectiveNumeric(*Numeric).UseConstant)
						{
							const auto& Values = EffectiveNumeric(*Numeric).Constant;
							// Uniform defaults are width-independent; retain authored components otherwise.
							if (Values.size() > 1 && !std::ranges::all_of(Values, [&](float V) { return V == Values.front(); }))
								Merge(static_cast<Type>(Values.size() - 1));
						}
					});
					if (bOperand) Result = Inferred;
					if (!GetMaterialProgramNodeSignature(Opcode, Result)) bValid = false;
					if (Property && *static_cast<Type*>(Property->GetValuePtr(E)) != Result)
					{ Write(*E, *static_cast<Type*>(Property->GetValuePtr(E)), Result); }
					VisitMaterialExpressionInputs(*E, [&](uint32 Slot, FMaterialExpressionInput& Operand) {
						if (Opcode == EMaterialProgramOpcode::Lerp && Slot == 2) return;
						if (auto* Numeric = FindMaterialNumericInput(*E, Operand))
						{
							const auto& Values = EffectiveNumeric(*Numeric).Constant;
							if (Values.size() > 1 && Values.size() != static_cast<size_t>(Result) + 1
								&& std::ranges::all_of(Values, [&](float V) { return V == Values.front(); })) { Write(*E, Numeric->Constant, std::vector<float>{Values.front()}); }
						}
					});
				}
				if (Result != PreviousType) ChangedTypes.insert(E);
				else if (!ForcedOutputs.contains(E) && (IsMaterialAdaptiveNumeric(Opcode) || Opcode == EMaterialProgramOpcode::AppendVector))
					ChangedTypes.erase(E);
				Active.erase(E);
				Resolved.emplace(E, Result);
				return Result;
			};
			for (const auto& E : Nodes)
			{
				const auto Shape = std::ranges::find(Catalog, E->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Affected.contains(E.Get()) && Shape != Catalog.end() && (IsMaterialAdaptiveNumeric(Shape->Opcode) || Shape->Opcode == EMaterialProgramOpcode::AppendVector)) Resolve({E.Get()});
			}
			return bValid;
		}

		auto AdaptNumericTypes(FGraphEditSession& State, std::span<DMaterialExpression* const> ChangedNodes, std::span<DMaterialExpression* const> ChangedOutputNodes) -> bool
		{
			return InferNumericTypes(State.Expressions, State.InferredNumericNodes, ChangedNodes, ChangedOutputNodes,
				[&](DMaterialExpression& E, auto& Field, auto Value) { State.Modify(E); Field = std::move(Value); });
		}

		auto EditExpressionInputs(DObject& Owner, DTransactor* Transactions, std::string Description,
			std::vector<DMaterialExpression*> Objects, const std::function<void()>& Apply,
			const FProposedLinks& Links, const FProposedNumerics& Numerics,
			std::map<DMaterialExpression*, EMaterialProgramValueType> KnownOutputs) -> FMaterialGraphCommandResult
		{
			if (auto Valid = ValidateGraphEdit(Owner, Links); !Valid) return Valid;
			std::vector<std::pair<EMaterialProgramValueType*, EMaterialProgramValueType>> Widths;
			std::vector<std::pair<std::vector<float>*, std::vector<float>>> Defaults;
			const auto Seeds = Objects;
			uint32 Count = 0;
			InferNumericTypes(FMaterialExpressionEditing::GetExpressions(Owner), Count, Seeds, {},
				[&](DMaterialExpression& Object, auto& Field, auto Value) {
					if (!std::ranges::contains(Objects, &Object)) Objects.push_back(&Object);
					if constexpr (std::is_same_v<decltype(Value), EMaterialProgramValueType>)
						Widths.emplace_back(&Field, Value);
					else Defaults.emplace_back(&Field, std::move(Value));
				}, Links, Numerics, std::move(KnownOutputs));
			return EditExpressions(Owner, Transactions, std::move(Description), Objects, [&] {
				Apply();
				for (auto& [Field, Width] : Widths) *Field = Width;
				for (auto& [Field, Default] : Defaults) *Field = std::move(Default);
			});
		}
	}

	auto GraphEditInternals::RemoveGraphNodes(DObject* Owner, std::span<const FGuid> NodeIds,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (NodeIds.size() > MaterialProgramMaxNodeCount) return RejectCommand("The removal selection exceeds the graph bound.");
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FGraphEditSession State(*Owner);
		const std::unordered_set<FGuid> Selection(NodeIds.begin(), NodeIds.end());
		std::unordered_set<DMaterialExpression*> Removed;
		for (const auto& Expression : State.Expressions)
		{
			if (!Selection.contains(Expression->Id)) continue;
			Removed.insert(Expression.Get());
		}
		if (!State.bFunction && !Removed.empty())
		{
			const auto Clear = [&](EMaterialProperty Property) {
				if (Removed.contains(State.GetRootInput(Property)->Expression.Get())) *State.EditRootInput(Property) = {};
			};
			Clear(EMaterialProperty::MP_MaterialAttributes);
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) Clear(Field.Property);
		}
		if (Removed.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		std::erase_if(State.Expressions, [&](const auto& Expression) { return Removed.contains(Expression.Get()); });
		std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Selection.contains(Position.NodeId); });
		for (auto& Expression : State.Expressions)
			VisitStoredMaterialInputs(*Expression, [&](FMaterialExpressionInput& Input) {
				if (!Removed.contains(Input.Expression.Get())) return;
				State.Modify(*Expression);
				Input = {};
			});
		return State.Commit("Remove Graph Nodes", Transactions);
	}

	auto DMaterialGraph::HasExpressionConsumer(const DMaterialExpression& Source) const -> bool
	{
		// During a compound edit the asset already contains the pending disconnection.
		// It also retains root inputs that have no visible graph pin.
		return std::ranges::any_of(FMaterialExpressionEditing::GetExpressions(*GetMaterialOrFunction()), [&](const auto& Consumer) {
			bool UsesSource = false;
			VisitStoredMaterialInputs(*Consumer, [&](const FMaterialExpressionInput& Input) { UsesSource |= Input.Expression.Get() == &Source; });
			return UsesSource;
		});
	}

	auto DMaterialGraph::RemoveNodes(std::span<const FGuid> NodeIds, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		if (std::ranges::contains(NodeIds, RootViewId)) return GraphEditInternals::RejectCommand("The material root cannot be removed.");
		return GraphEditInternals::RemoveGraphNodes(GetMaterialOrFunction(), NodeIds, Mode);
	}

	namespace
	{
		auto PinGraph(const FMaterialGraphPin* Pin) -> DMaterialGraph*
		{
			if (!Pin) return nullptr;
			auto* Node = Pin->GetOwningNode();
			const auto* Current = Pin->Direction == EMaterialGraphPinDirection::Input
				? Node->GetInputPin(Pin->DisplayIndex) : Node->GetOutputPin(Pin->DisplayIndex);
			auto* Graph = Cast<DMaterialGraph>(Node->GetOuter());
			return Current == Pin && Graph && (Node->GetMaterialExpression() || Graph->GetRootNode() == Node) ? Graph : nullptr;
		}

		auto CheckConnection(const FMaterialGraphPin& Output, const DMaterialGraphNode& Target, bool bTypesCompatible,
			const FMaterialGraphPin* Existing) -> FMaterialGraphConnectionResponse
		{
			using R = EMaterialGraphConnectionResponse;
			const auto* Source = Output.GetOwningNode();
			if (Existing && std::ranges::contains(Existing->LinkedTo, &Output)) return {R::Make, {}};
			if (!bTypesCompatible) return {R::Disallow, "The pin types are incompatible."};
			std::vector<const DMaterialGraphNode*> Pending{Source};
			std::unordered_set<const DMaterialGraphNode*> Visited;
			while (!Pending.empty())
			{
				const auto* Node = Pending.back();
				Pending.pop_back();
				if (Node == &Target) return {R::Disallow, "The connection would create a cycle."};
				if (!Visited.insert(Node).second) continue;
				for (const auto& Pin : Node->GetPins())
					if (Pin.Direction == EMaterialGraphPinDirection::Input)
						for (const auto* Upstream : Pin.LinkedTo) Pending.push_back(Upstream->GetOwningNode());
			}
			if (Existing && !Existing->LinkedTo.empty()) return {R::ReplaceInput, "The input already has a connection."};
			return {R::Make, {}};
		}

		auto ConnectionCommitChanged(const FMaterialGraphCommandResult& Result, std::string* OutError) -> bool
		{
			if (OutError) *OutError = FormatMaterialGraphCommandResult(Result);
			return Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded;
		}
	}

	auto FMaterialGraphSchema::CanCreateConnection(const FMaterialGraphPin* A, const FMaterialGraphPin* B) const -> FMaterialGraphConnectionResponse
	{
		using R = EMaterialGraphConnectionResponse;
		const auto* Graph = PinGraph(A);
		if (!Graph || PinGraph(B) != Graph) return {R::Disallow, "Both pins must belong to the same live graph."};
		if (A->Direction == B->Direction) return {R::Disallow, "Connect an output to an input."};
		const auto* Input = A->Direction == EMaterialGraphPinDirection::Input ? A : B;
		const auto* Node = Input->GetOwningNode();
		const auto* Output = Input == A ? B : A;
		const auto Type = Output->GetOwningNode()->GetView().Outputs[Output->DisplayIndex].Type;
		return CheckConnection(*Output, *Node, IsConnectionTypeCompatible(Node->GetView().Inputs[Input->DisplayIndex], Type), Input);
	}

	auto FMaterialGraphSchema::TryCreateConnection(const FMaterialGraphPin* A, const FMaterialGraphPin* B,
		bool Replace, ETransactionMode Mode, std::string* OutError) const -> bool
	{
		const auto Response = CanCreateConnection(A, B);
		if (!Response.Allows(Replace))
		{
			if (OutError) *OutError = Response.Reason;
			return false;
		}
		const auto* Input = A->Direction == EMaterialGraphPinDirection::Input ? A : B;
		return PinGraph(Input)->LinkMaterialExpressionsFromGraph(*Input, Input == A ? B : A, Replace, Mode, OutError);
	}

	auto FMaterialGraphSchema::BreakPinLinks(const FMaterialGraphPin* Pin, ETransactionMode Mode, std::string* OutError) const -> bool
	{
		auto* Graph = PinGraph(Pin);
		if (!Graph) return GraphEditInternals::FailWithError(OutError, "The pin is unavailable.");
		if (Pin->Direction == EMaterialGraphPinDirection::Output) return Graph->BreakOutputLinks(*Pin, Mode, OutError);
		return Graph->LinkMaterialExpressionsFromGraph(*Pin, nullptr, true, Mode, OutError);
	}

	auto FMaterialGraphSchema::CanCreateConnection(const DMaterialGraph& Graph, const FMaterialGraphPinAddress& A,
		const FMaterialGraphPinAddress& B) const -> FMaterialGraphConnectionResponse
	{
		return CanCreateConnection(Graph.FindPin(A), Graph.FindPin(B));
	}

	auto FMaterialGraphSchema::TryCreateConnection(DMaterialGraph& Graph, const FMaterialGraphPinAddress& A,
		const FMaterialGraphPinAddress& B, bool Replace, ETransactionMode Mode, std::string* OutError) const -> bool
	{
		return TryCreateConnection(Graph.FindPin(A), Graph.FindPin(B), Replace, Mode, OutError);
	}

	auto FMaterialGraphSchema::BreakPinLinks(DMaterialGraph& Graph, FMaterialGraphPinAddress Pin, ETransactionMode Mode, std::string* OutError) const -> bool
	{
		return BreakPinLinks(Graph.FindPin(Pin), Mode, OutError);
	}

	auto DMaterialGraphNode::CanSetAttributeInput(EMaterialProperty Property, const FMaterialGraphPin* Output) const -> FMaterialGraphConnectionResponse
	{
		auto* Graph = PinGraph(Output);
		if (!Graph || Graph != GetOuter() || Output->Direction != EMaterialGraphPinDirection::Output
			|| !Cast<DMaterialExpressionSetMaterialAttributes>(GetMaterialExpression()) || !FMaterialAttributeDefinitionMap::Find(Property))
			return {EMaterialGraphConnectionResponse::Disallow, "The attribute input or source output is unavailable."};
		const auto Address = FMaterialGraphPinAddress::Input(View.Node.Id, static_cast<uint32>(Property) + 1);
		if (const auto* Input = Graph->FindPin(Address)) return Graph->GetSchema().CanCreateConnection(Output, Input);
		const auto SourceType = Output->GetOwningNode()->GetView().Outputs[Output->DisplayIndex].Type;
		const auto Type = GetMaterialSurfaceOutputType(Property);
		const bool Compatible = SourceType == Type || (MaterialInputAllowsValidCast(EMaterialProgramOpcode::MakeMaterialAttributes, uint32(Property))
			&& FMaterialGraphSchema::AcceptsPort(Type, SourceType));
		return CheckConnection(*Output, *this, Compatible, nullptr);
	}

	auto DMaterialGraphNode::SetAttributeInput(EMaterialProperty Property, const FMaterialGraphPin& Output,
		ETransactionMode Mode, std::string* OutError) -> bool
	{
		using namespace GraphEditInternals;
		const auto Response = CanSetAttributeInput(Property, &Output);
		if (!Response.Allows(true)) return FailWithError(OutError, Response.Reason);
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		const auto Address = FMaterialGraphPinAddress::Input(View.Node.Id, static_cast<uint32>(Property) + 1);
		if (const auto* Input = Graph->FindPin(Address))
			return Graph->GetSchema().TryCreateConnection(&Output, Input, true, Mode, OutError);

		auto Transactions = ResolveEditorTransactor(Mode);
		if (!Transactions) return FailWithError(OutError, Transactions.error());
		auto* Target = Cast<DMaterialExpressionSetMaterialAttributes>(GetMaterialExpression());
		auto* Source = Output.GetOwningNode();
		const auto& Pin = Source->GetView().Outputs[Output.DisplayIndex];
		FGraphEditSession Edit(*Target->GetOuter());
		auto* Upstream = Source->GetMaterialExpression();
		Edit.IncludeCallOutput(Upstream, Pin.PortId);
		const FMaterialExpressionInput Link{Upstream, Pin.OutputIndex, Pin.PortId};
		if (Link.OutputIndex < 0) return FailWithError(OutError, "The source output is unavailable.");
		Edit.Modify(*Target);
		Target->Attributes.emplace_back(Property, Link);
		return ConnectionCommitChanged(Edit.Commit("Set Material Attribute Input", *Transactions), OutError);
	}

	auto DMaterialGraphNode::ClearAttributeInput(EMaterialProperty Property, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto* Target = Cast<DMaterialExpressionSetMaterialAttributes>(GetMaterialExpression());
		if (!Target || !FMaterialAttributeDefinitionMap::Find(Property)) return RejectCommand("The attribute input is unavailable.");
		const auto It = std::ranges::find(Target->Attributes, Property, &FMaterialExpressionAttributeBinding::GetProperty);
		if (It == Target->Attributes.end()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		auto Transactions = ResolveEditorTransactor(Mode);
		if (!Transactions) return RejectCommand(Transactions.error());
		FGraphEditSession Edit(*Target->GetOuter());
		Edit.Modify(*Target);
		Target->Attributes.erase(It);
		return Edit.Commit("Clear Material Attribute Input", *Transactions);
	}

	auto DMaterialGraph::BreakOutputLinks(const FMaterialGraphPin& Output, ETransactionMode Mode, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		using namespace GraphEditInternals;
		auto* Owner = GetMaterialOrFunction();
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return GraphEditInternals::FailWithError(OutError, Admission.error());
		auto* Upstream = Output.GetOwningNode()->GetMaterialExpression();
		struct FConsumer
		{
			DMaterialExpression* Expression;
			FMaterialExpressionInput* Input;
		};
		std::vector<FConsumer> Consumers;
		std::unordered_set<FMaterialExpressionInput*> Seen;
		const auto Add = [&](DMaterialExpression& Target, FMaterialExpressionInput& Input) {
			if (Input.Expression.Get() != Upstream || Input.OutputIndex != Output.SourceIndex || !Seen.insert(&Input).second) return;
			Consumers.push_back({&Target, &Input});
		};
		for (const auto* Pin : Output.LinkedTo)
		{
			auto* Target = Pin->GetOwningNode()->GetMaterialExpression();
			if (!Target || Pin->SourceIndex < 0) continue;
			VisitMaterialExpressionInputs(*Target, [&](uint32 Index, FMaterialExpressionInput& Input) {
				if (Index != static_cast<uint32>(Pin->SourceIndex)) return;
				Add(*Target, Input);
			});
		}
		// Switching root modes hides retained inputs. Breaking an output also clears those consumers.
		if (auto* Material = Cast<DMaterial>(Owner))
		{
			FGraphEditSession Edit(*Material);
			bool RootChanged = false;
			const auto Clear = [&](EMaterialProperty Property) {
				const auto* Link = Edit.GetRootInput(Property);
				if (Link->Expression.Get() == Upstream && Link->OutputIndex == Output.SourceIndex)
				{
					*Edit.EditRootInput(Property) = {};
					RootChanged = true;
				}
			};
			Clear(EMaterialProperty::MP_MaterialAttributes);
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) Clear(Field.Property);
			if (RootChanged)
			{
				for (const auto& Consumer : Consumers) { Edit.Modify(*Consumer.Expression); *Consumer.Input = {}; }
				return ConnectionCommitChanged(Edit.Commit("Break Output Pin Links", *Admission), OutError);
			}
		}
		if (Consumers.empty()) return false;
		std::vector<DMaterialExpression*> Objects;
		FProposedLinks Links;
		for (const auto& Consumer : Consumers)
		{
			if (!std::ranges::contains(Objects, Consumer.Expression)) Objects.push_back(Consumer.Expression);
			Links.emplace(Consumer.Input, FMaterialExpressionInput{});
		}
		const auto Result = EditExpressionInputs(*Owner, *Admission, "Break Output Pin Links", std::move(Objects),
			[&] { for (const auto& Consumer : Consumers) *Consumer.Input = {}; }, Links);
		return ConnectionCommitChanged(Result, OutError);
	}

	auto DMaterialGraphNode::GetMaterialExpression() const -> DMaterialExpression*
	{
		if (ActiveSource) return ActiveSource->Expression;
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Graph->FindNode(View.Node.Id) != this) return nullptr;
		auto* Expression = MaterialExpression.Get();
		auto* Owner = Graph->GetMaterialOrFunction();
		if (!Owner || !Expression || Expression->GetOuter() != Owner) return nullptr;
		const auto& Members = FMaterialExpressionEditing::GetExpressions(*Owner);
		return std::ranges::contains(Members, Expression, [](const auto& Member) { return Member.Get(); }) ? Expression : nullptr;
	}

	auto DMaterialGraphNode::ResolveInputGraph(const FMaterialGraphPinAddress& Pin) const -> DMaterialGraph*
	{
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Pin.IsMaterialRoot() != View.Node.bMaterialOutput || (!Pin.IsMaterialRoot() && Pin.NodeId != View.Node.Id) || Graph->FindNode(View.Node.Id) != this) return nullptr;
		// A material retains numeric property defaults while its root shows the Surface pin.
		if (Pin.HasValidRootAddress() && FMaterialAttributeDefinitionMap::Find(Pin.Property)) return Graph;
		return Graph->FindInputPin(Pin) ? Graph : nullptr;
	}

	auto DMaterialGraphNode::PinDefaultValueChanged(const FMaterialGraphPinAddress& Pin, FMaterialInputDefault Value,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto* Graph = ResolveInputGraph(Pin);
		if (!Graph) return GraphEditInternals::RejectCommand("The input pin is unavailable.");
		return GraphEditInternals::WriteInputDefault(Graph->GetMaterialOrFunction(),
			Pin, std::move(Value), Mode);
	}

	auto DMaterialGraphNode::SetPinUseConstant(const FMaterialGraphPinAddress& Pin, bool Enabled,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto* Graph = ResolveInputGraph(Pin);
		if (!Graph || Pin.PortId.IsValid()) return GraphEditInternals::RejectCommand("The numeric input pin is unavailable.");
		return GraphEditInternals::SetInputConstantEnabled(Graph->GetMaterialOrFunction(),
			Pin, Enabled, Mode);
	}

	auto DMaterialGraph::Create(DObject& MaterialOrFunction) -> DMaterialGraph*
	{
		if (!Cast<DMaterial>(&MaterialOrFunction) && !Cast<DMaterialFunction>(&MaterialOrFunction)) return nullptr;
		auto* Graph = NewObject<DMaterialGraph>(StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Graph->Owner = &MaterialOrFunction;
		Graph->Schema = &FMaterialGraphSchema::ForOwner(MaterialOrFunction);
		Graph->OwnerHandle = GetMaterialGraphChangeSource(MaterialOrFunction).Subscribe(MaterialOrFunction,
			[WeakGraph = TWeakObjectPtr<DMaterialGraph>(Graph)](const auto& Changes) {
				if (auto* Live = WeakGraph.Get()) Live->ApplyChanges(Changes);
			});
		Graph->RebuildGraph();
		return Graph;
	}

	auto DMaterialGraph::GetSchema() const -> const FMaterialGraphSchema& { return *Schema; }

	auto DMaterialGraph::FindNode(const FGuid& Id) const -> DMaterialGraphNode*
	{
		for (const auto& Node : Nodes)
			if (Node->GetView().Node.Id == Id) return Node.Get();
		return nullptr;
	}

	auto DMaterialGraph::FindInputPin(const FMaterialGraphPinAddress& Address) const -> const FMaterialGraphPinView*
	{
		const auto* Node = (Address.IsMaterialRoot() ? GetRootNode() : FindNode(Address.NodeId));
		if (!Node) return nullptr;
		const auto& View = Node->GetView();
		for (const auto& Input : View.Inputs)
			if (View.InputAddress(Input) == Address) return &Input;
		return nullptr;
	}

	auto DMaterialGraph::FindPin(const FMaterialGraphPinAddress& Address) const -> const FMaterialGraphPin*
	{
		auto* Node = (Address.IsMaterialRoot() ? GetRootNode() : FindNode(Address.NodeId));
		if (!Node) return nullptr;
		const auto& View = Node->GetView();
		if (IsOutput(Address))
		{
			if (!FMaterialGraphSchema::IsSourceAddressValid(Address) || !FMaterialGraphSchema::IsSourceKeyValid(Address)) return nullptr;
			for (size_t I = 0; I < View.Outputs.size(); ++I)
				if (View.Outputs[I].PortId == Address.PortId && View.Outputs[I].OutputIndex == Address.Index)
					return Node->GetOutputPin(static_cast<int32>(I));
		}
		else for (size_t I = 0; I < View.Inputs.size(); ++I)
			if (View.InputAddress(View.Inputs[I]) == Address) return Node->GetInputPin(static_cast<int32>(I));
		return nullptr;
	}

	namespace
	{
		using namespace GraphEditInternals;
		auto IsSourceAvailable(std::span<const TObjectPtr<DMaterialExpression>> Expressions, const FMaterialExpressionInput& Input) -> bool
		{
			if (!Input.IsConnected()) return Input.OutputIndex == 0;
			if (Input.OutputIndex < 0) return false;
			const auto It = std::ranges::find(Expressions, Input.Expression);
			if (It == Expressions.end()) return false;
			const auto* E = It->Get();
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(E))
				return Call->Function.IsValid() && Input.GetOutputId().IsValid()
					&& std::ranges::any_of(Call->Function->GetFunctionSignature().Outputs, [&](const auto& Port) { return Port.Id == Input.GetOutputId(); });
			if (const auto* Attributes = Cast<DMaterialExpressionGetMaterialAttributes>(E))
			{
				const auto* Definition = FMaterialAttributeDefinitionMap::Find(Input.GetOutputId());
				return Definition && Attributes->IsSelected(Definition->Property);
			}
			if (Input.GetOutputId().IsValid() || Cast<DMaterialExpressionFunctionOutput>(E)) return false;
			if (Cast<DMaterialExpressionTextureSampleParameter2D>(E))
				return FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSampleParameter2D, Input.OutputIndex) != nullptr;
			if (Cast<DMaterialExpressionTextureSampleParameterCube>(E)) return FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSampleParameterCube, Input.OutputIndex) != nullptr;
			if (Cast<DMaterialExpressionTextureSampleCube>(E)) return FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSampleCube, Input.OutputIndex) != nullptr;
			if (Cast<DMaterialExpressionTextureSample2D>(E))
				return FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSample2D, Input.OutputIndex) != nullptr;
			return Input.OutputIndex == 0;
		}

		auto WriteRootInput(DMaterial* Material, FMaterialGraphPinAddress Address, DMaterialExpression* Source, int32 Index, FGuid Port,
			bool Replace, ETransactionMode Mode, std::string* Error) -> bool
		{
			auto Admission = ResolveEditorTransactor(Mode);
			if (!Admission) return FailWithError(Error, Admission.error());
			if (!Material || !Address.HasValidRootAddress()) return FailWithError(Error, "Invalid material root address.");
			FGraphEditSession Edit(*Material);
			auto* Input = Edit.EditRootInput(Address.Property);
			if (!Input) return FailWithError(Error, "The material attribute pin is invalid.");
			Edit.IncludeCallOutput(Source, Port);
			const FMaterialExpressionInput Next{Source, Port.IsValid() ? 0 : Index, Port};
			if (Next.OutputIndex < 0) return FailWithError(Error, "The source output no longer exists.");
			if (!FMaterialGraphSchema::CanReplaceConnection(*Input, Next, Replace)) return FailWithError(Error, "The graph input is already connected.");
			*Input = Next;
			return ConnectionCommitChanged(Edit.Commit("Connect Material Input", *Admission), Error);
		}

		auto WriteInput(DObject* Owner, DMaterialExpression& Node, FMaterialGraphPinAddress TargetAddress,
			DMaterialExpression* Upstream, int32 SourceIndex, FGuid UnboundPort, bool bReplaceExisting, ETransactionMode Mode, std::string* OutError) -> bool
		{
			if (OutError) OutError->clear();
			auto Admission = ResolveEditorTransactor(Mode);
			if (!Admission) return GraphEditInternals::FailWithError(OutError, Admission.error());
			auto* Transactions = *Admission;
			if (!Owner) return GraphEditInternals::FailWithError(OutError, "The material graph owner is no longer available.");
			FMaterialExpressionInput* Target = nullptr;
			auto* Call = Cast<DMaterialExpressionFunctionCall>(&Node);
			const FMaterialFunctionPort* Port = nullptr;
			if (TargetAddress.Kind == EMaterialGraphPinKind::FunctionInput)
			{
				if (!Call || !TargetAddress.PortId.IsValid()) return GraphEditInternals::FailWithError(OutError, "The function call is unavailable.");
				if (IsValid(Call->Function.Get()))
				{
					const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
					const auto Found = std::ranges::find(Ports, TargetAddress.PortId, &FMaterialFunctionPort::Id);
					if (Found != Ports.end()) Port = &*Found;
				}
				if (!Port) return GraphEditInternals::FailWithError(OutError, "The function input port no longer exists.");
				const auto Binding = std::ranges::find(Call->FunctionInputs, TargetAddress.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				if (Binding != Call->FunctionInputs.end())
				{
					Target = &Binding->Input;
				}
			}
			else
			{
				if (Call || TargetAddress.PortId.IsValid()) return GraphEditInternals::FailWithError(OutError, "The input pin key is invalid.");
				if (TargetAddress.Kind != EMaterialGraphPinKind::Input) return FailWithError(OutError, "The target must be an expression input pin.");
				const uint32 Index = TargetAddress.Index;
				VisitMaterialExpressionInputs(Node, [&](uint32 Slot, FMaterialExpressionInput& Input) { if (Slot == Index) Target = &Input; });
				if (!Target) return GraphEditInternals::FailWithError(OutError, "The graph input no longer exists.");
			}
			DMaterialExpressionFunctionCall* SourceCall = nullptr;
			FMaterialFunctionOutputBinding NewOutput;
			if (UnboundPort.IsValid())
			{
				SourceCall = Cast<DMaterialExpressionFunctionCall>(Upstream);
				if (!SourceCall || !SourceCall->Function.IsValid()) return GraphEditInternals::FailWithError(OutError, "The function output is unavailable.");
				const auto& Ports = SourceCall->Function->GetFunctionSignature().Outputs;
				const auto Found = std::ranges::find(Ports, UnboundPort, &FMaterialFunctionPort::Id);
				if (Found == Ports.end()) return GraphEditInternals::FailWithError(OutError, "The function output no longer exists.");
				if (std::ranges::contains(SourceCall->FunctionOutputs, UnboundPort, &FMaterialFunctionOutputBinding::OutputId))
					return GraphEditInternals::FailWithError(OutError, "The source output binding has changed.");
				NewOutput = {Found->Id, Found->Type};
				SourceIndex = static_cast<int32>(SourceCall->FunctionOutputs.size());
			}
			FMaterialExpressionInput Connection;
			Connection.Connect(SourceIndex, Upstream);
			const auto Previous = Target ? *Target : FMaterialExpressionInput{};
			if (!FMaterialGraphSchema::CanReplaceConnection(Previous, Connection, bReplaceExisting))
				return GraphEditInternals::FailWithError(OutError, "The graph input is already connected.");
			if (Previous == Connection) return false;
			FMaterialGraphCommandResult Result;
			if (Target && !SourceCall)
			{
				Result = EditExpressionInputs(*Owner, Transactions, "Connect Graph Input", {&Node},
					[&] { *Target = Connection; }, {{Target, Connection}});
			}
			else
			{
				FGraphEditSession State(*Owner);
				if (SourceCall)
				{
					State.Modify(*SourceCall);
					SourceCall->FunctionOutputs.push_back(NewOutput);
				}
				State.Modify(Node);
				if (Target) *Target = Connection;
				else Call->FunctionInputs.push_back({Port->Id, Port->Type, Connection});
				Result = State.Commit("Connect Graph Input", Transactions);
			}
			return ConnectionCommitChanged(Result, OutError);
		}

	}

	auto DMaterialGraph::LinkMaterialExpressionsFromGraph(const FMaterialGraphPin& Input, const FMaterialGraphPin* Output,
		bool Replace, ETransactionMode Mode, std::string* OutError) -> bool
	{
		auto* Node = Input.GetOwningNode();
		const auto Target = Node->GetView().InputAddress(Node->GetView().Inputs[Input.DisplayIndex]);
		auto* Expression = Node->GetMaterialExpression();
		if (Target.IsMaterialRoot())
		{
			const auto* Source = Output ? Output->GetOwningNode() : nullptr;
			return WriteRootInput(Cast<DMaterial>(GetMaterialOrFunction()), Target, Source ? Source->GetMaterialExpression() : nullptr,
				Output ? Output->SourceIndex : 0, Output ? Source->GetView().Outputs[Output->DisplayIndex].PortId : FGuid{}, Replace, Mode, OutError);
		}
		if (!Expression) return GraphEditInternals::FailWithError(OutError, "The graph input is unavailable.");
		if (!Output) return WriteInput(GetMaterialOrFunction(), *Expression, Target, nullptr, 0, {}, Replace, Mode, OutError);
		auto* Source = Output->GetOwningNode();
		auto* Upstream = Source->GetMaterialExpression();
		if (!Upstream) return GraphEditInternals::FailWithError(OutError, "The source output no longer exists.");
		const auto PortId = Source->GetView().Outputs[Output->DisplayIndex].PortId;
		const auto Index = Output->SourceIndex;
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Upstream); Call && Index >= 0)
			if (static_cast<size_t>(Index) >= Call->FunctionOutputs.size() || Call->FunctionOutputs[Index].OutputId != PortId)
				return GraphEditInternals::FailWithError(OutError, "The source output binding has changed.");
		return WriteInput(GetMaterialOrFunction(), *Expression, Target, Upstream, Index, Index < 0 ? PortId : FGuid{}, Replace, Mode, OutError);
	}

	// Low-level repair writes can target retained root inputs and incomplete graphs.
	auto GraphEditInternals::WriteExpressionConnection(DObject* Owner, const FMaterialGraphPinAddress& Target,
		const FMaterialGraphPinAddress& Source, bool Replace, ETransactionMode Mode, std::string* OutError) -> bool
	{
		if (!Owner) return GraphEditInternals::FailWithError(OutError, "The graph owner is unavailable.");
		auto* Node = ResolveExpression(Owner, Target.NodeId);
		if (!FMaterialGraphSchema::IsSourceAddressValid(Source)) return GraphEditInternals::FailWithError(OutError, "The source must be an output pin.");
		if (!FMaterialGraphSchema::IsSourceKeyValid(Source)) return GraphEditInternals::FailWithError(OutError, "The source pin key is invalid.");
		if (!Node && !Target.IsMaterialRoot()) return GraphEditInternals::FailWithError(OutError, "The graph input no longer exists.");
		auto* Upstream = ResolveExpression(Owner, Source.NodeId);
		if (Source.NodeId.IsValid() && !Upstream) return GraphEditInternals::FailWithError(OutError, "The source output no longer exists.");
		const FMaterialExpressionInput Input{Upstream, static_cast<int32>(Source.Index), Source.PortId};
		const auto Unbound = Cast<DMaterialExpressionFunctionCall>(Upstream) && Input.OutputIndex < 0 ? Source.PortId : FGuid{};
		if (!Unbound.IsValid() && !IsSourceAvailable(FMaterialExpressionEditing::GetExpressions(*Owner), Input))
			return GraphEditInternals::FailWithError(OutError, "The source output no longer exists.");
		if (Target.IsMaterialRoot()) return WriteRootInput(Cast<DMaterial>(Owner), Target, Upstream, Input.OutputIndex, Source.PortId, Replace, Mode, OutError);
		return WriteInput(Owner, *Node, Target, Upstream, Input.OutputIndex, Unbound, Replace, Mode, OutError);
	}

	auto DMaterialGraph::FindOutputPin(const FMaterialProgramLink& Link) const -> const FMaterialGraphOutputPinView*
	{
		const auto* Node = FindNode(Link.SourceNodeId);
		if (!Node) return nullptr;
		for (const auto& Pin : Node->GetView().Outputs)
			if (Pin.PortId == Link.SourceOutputId && (Pin.PortId.IsValid() || Pin.OutputIndex == Link.SourceOutputIndex))
				return &Pin;
		return nullptr;
	}

	auto DMaterialGraph::AddExpression(DMaterialExpression& Expression) -> DMaterialGraphNode*
	{
		if (ResolveExpression(GetMaterialOrFunction(), Expression.Id) != &Expression) return nullptr;
		if (auto* Node = FindNode(Expression.Id)) return Node;
		FMaterialGraphChangeSet Changes;
		Changes.MarkNode(Expression.Id, EMaterialGraphNodeChange::Added);
		ApplyChanges(std::move(Changes));
		return FindNode(Expression.Id);
	}

	auto DMaterialGraph::RebuildGraph() -> void
	{
		for (const auto& Node : Nodes)
			Node->ClearPinLinks();
		Nodes.clear();
		FMaterialGraphChangeSet Changes;
		Changes.MarkGraph(EMaterialGraphChange::Reset);
		ApplyChanges(std::move(Changes));
	}

	auto DMaterialGraph::RefreshNodes(std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::optional<std::span<const FGuid>> Selected) -> void
	{
		std::unordered_map<FGuid, DMaterialGraphNode*> Existing;
		for (const auto& Node : Nodes) Existing.emplace(Node->GetView().Node.Id, Node.Get());
		std::vector<TObjectPtr<DMaterialGraphNode>> Updated;
		auto Summary = ReadMaterialGraph(GetMaterialOrFunction(), RootViewId, Catalog, Selected,
			[&](FMaterialGraphNodeView Metadata, const FMaterialGraphPinSource& Source) {
				const auto It = Existing.find(Metadata.Node.Id);
				const bool NewNode = It == Existing.end() || It->second->GetView().Node.bMaterialOutput != Metadata.Node.bMaterialOutput;
				auto* Type = Metadata.Node.bMaterialOutput ? DMaterialGraphNode_Root::StaticClass() : DMaterialGraphNode::StaticClass();
				auto* Node = NewNode ? NewObject<DMaterialGraphNode>(Type, this, NAME_None, EObjectFlags::Transient) : It->second;
				if (NewNode)
				{
					if (It != Existing.end())
					{
						It->second->ClearPinLinks();
						std::erase_if(Nodes, [&](const auto& Value) { return Value.Get() == It->second; });
					}
					Nodes.emplace_back(Node);
				}
				Node->Refresh(std::move(Metadata), Source, NewNode ? &DMaterialGraphNode::AllocateDefaultPins : &DMaterialGraphNode::ReconstructNode);
				if (!Selected) Updated.emplace_back(Node);
			});
		if (!Selected)
		{
			// Detached nodes may still be held by an editor client.
			for (const auto& Node : Nodes)
				Node->ClearPinLinks();
			Nodes = std::move(Updated);
		}
		std::ranges::sort(Nodes, {}, [](const auto& Node) { return Node->GetView().Node.Id; });
		Outputs = std::move(Summary.Outputs);
		RestorePinLinks();
	}

	auto DMaterialGraph::LinkGraphNodesFromMaterial() -> void
	{
		RestorePinLinks();
		if (const auto* Material = Cast<DMaterial>(Owner.Get())) Outputs = ReadGraphOutputs(*Material);
		FMaterialGraphChangeSet Changes;
		Changes.MarkGraph(EMaterialGraphChange::Reset);
		Synchronized.Broadcast(Changes);
	}

	auto DMaterialGraph::RestorePinLinks() -> void
	{
		struct FOutput
		{
			FMaterialGraphPin* Pin;
			const FMaterialGraphOutputPinView* View;
		};
		std::unordered_map<const DMaterialExpression*, std::unordered_map<int32, FOutput>> Sources;
		for (const auto& Node : Nodes)
		{
			auto& Slots = Sources[Node->MaterialExpression.Get()];
			Node->ClearPinLinks();
			for (size_t I = 0; I < Node->View.Outputs.size(); ++I)
			{
				auto& Pin = Node->Pins[Node->View.Inputs.size() + I];
				if (Pin.SourceIndex >= 0) Slots.emplace(Pin.SourceIndex, FOutput{&Pin, &Node->View.Outputs[I]});
			}
		}

		for (const auto& Node : Nodes)
		{
			auto* Expression = Node->MaterialExpression.Get();
			if (Node->View.Inputs.empty()) continue;
			std::unordered_map<int32, size_t> Inputs;
			for (size_t I = 0; I < Node->View.Inputs.size(); ++I)
			{
				auto& View = Node->View.Inputs[I];
				if (View.Link.SourceNodeId.IsValid())
				{
					View.Link = {};
					View.SourceType = View.InlineDefault.Kind != EMaterialInputDefaultKind::None ? View.InlineDefault.Type
						: View.AcceptedTypes.empty() ? EMaterialProgramValueType::Float : View.AcceptedTypes.front();
				}
				if (Node->Pins[I].SourceIndex >= 0) Inputs.emplace(Node->Pins[I].SourceIndex, I);
			}

			const auto Restore = [&](uint32 Index, const FMaterialExpressionInput& Input) {
				if (!Input.IsConnected()) return;
				const auto Target = Inputs.find(static_cast<int32>(Index));
				const auto Source = Sources.find(Input.Expression.Get());
				if (Target == Inputs.end() || Source == Sources.end()) return;
				const auto Output = Source->second.find(Input.OutputIndex);
				if (Output == Source->second.end()) return;
				const auto& [Pin, View] = Output->second;
				Node->Pins[Target->second].MakeLinkTo(*Pin);
				auto& InputView = Node->View.Inputs[Target->second];
				InputView.Link = {Pin->GetOwningNode()->View.Node.Id, View->OutputIndex, View->PortId};
				InputView.SourceType = View->Type;
			};
			if (Expression) VisitMaterialExpressionInputs(*Expression, Restore);
			else if (const auto* Material = Cast<DMaterial>(Owner.Get()))
				for (const auto& Pin : Node->View.Inputs)
				{
					if (const auto Description = Material->GetExpressionInputDescription(*Pin.MaterialProperty)) Restore(Pin.InputIndex, *Description->Connection);
				}
		}
	}
}
