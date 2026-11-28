#include "MaterialGraph.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/Material.h"
#include "DObject/ObjectLifecycle.h"
#include "Graph/MaterialGraphCommands.h"
#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpressionIO.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto ToGraphLink(const FMaterialExpressionInput& Input) -> FMaterialProgramLink
		{
			const FGuid Port = Input.GetOutputId();
			if (!Port.IsValid() && !std::in_range<uint8>(Input.OutputIndex)) return {};
			return {Input.GetExpressionId(), static_cast<uint8>(Port.IsValid() ? 0 : Input.OutputIndex), Port};
		}

		auto ReadGraphOutputs(const DMaterial& Material) -> FMaterialAttributeValues
		{
			FMaterialAttributeValues Result;
			Result.Attributes = ToGraphLink(*Material.GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes));
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
			{
				const auto Input = *Material.GetNumericInputForProperty(Definition.Property);
				GetMaterialSurfaceOutputLink(Result, Definition.Property) = ToGraphLink(Input.Connection);
				const auto V = ReadMaterialNumericDefault(Input, Definition.Property);
				GetMaterialSurfaceOutputDefault(Result, Definition.Property) = {V[0], V.size() > 1 ? V[1] : 0.f, V.size() > 2 ? V[2] : 0.f};
			}
			return Result;
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
			std::map<DMaterialExpression*, EMaterialProgramValueType> Resolved = {}, const FProposedCallOutputs& Outputs = {}) -> bool
		{
			using Type = EMaterialProgramValueType;
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
					const auto Proposed = Outputs.find(const_cast<DMaterialExpressionFunctionCall*>(Call));
					if (Proposed != Outputs.end() && Input.OutputIndex >= 0
						&& static_cast<size_t>(Input.OutputIndex) < Proposed->second.size())
						return Proposed->second[Input.OutputIndex].ExpectedType;
				}
				const auto Pin = FindExpressionOutputPin(*E, Input.OutputIndex);
				if (!Pin) { bValid = false; return Type::Float; }
				const auto Operation = E->GetCreationInfo().Opcode;
				// Inferred widths describe the primary numeric result only.
				if (!Operation || Pin->SourceIndex != 0 || Pin->Output.PortId.IsValid()) return Pin->Output.Type;
				const auto Opcode = *Operation;
				if (Resolved.contains(E)) return Resolved.at(E);
				if (!Active.insert(E).second) { bValid = false; return Type::Float; }
				auto* AuthoredType = E->GetAuthoredResultType();
				auto Result = Pin->Output.Type;
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
						if (AuthoredType && *AuthoredType != Inferred) Write(*Append, *AuthoredType, Inferred);
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
					if (AuthoredType && *AuthoredType != Result) Write(*E, *AuthoredType, Result);
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
				if (!Affected.contains(E.Get())) continue;
				const auto Opcode = E->GetCreationInfo().Opcode;
				if (Opcode && (IsMaterialAdaptiveNumeric(*Opcode) || *Opcode == EMaterialProgramOpcode::AppendVector)) Resolve({E.Get()});
			}
			return bValid;
		}

		namespace
		{
			struct FInferredNumericEdits
			{
				std::vector<DMaterialExpression*> Objects;
				std::vector<std::pair<EMaterialProgramValueType*, EMaterialProgramValueType>> Widths;
				std::vector<std::pair<std::vector<float>*, std::vector<float>>> Defaults;

				template<class T>
				auto Add(DMaterialExpression& Expression, T& Field, T Value) -> void
				{
					Objects.push_back(&Expression);
					if constexpr (std::is_same_v<T, EMaterialProgramValueType>) Widths.emplace_back(&Field, Value);
					else Defaults.emplace_back(&Field, std::move(Value));
				}

				auto Write() -> void
				{
					for (const auto& [Field, Width] : Widths) *Field = Width;
					for (auto& [Field, Default] : Defaults) *Field = std::move(Default);
				}
			};

			auto InferStructuralEdit(DObject& Owner, const FMaterialExpressionCollection& Collection,
				const FProposedLinks& Links, const FProposedNumerics& Numerics, const FProposedCallOutputs& Outputs) -> FInferredNumericEdits
			{
				const auto& Current = FMaterialExpressionEditing::GetExpressions(Owner);
				std::unordered_set<DMaterialExpression*> Live;
				for (const auto& Expression : Current) Live.insert(Expression.Get());
				std::vector<DMaterialExpression*> Seeds, ChangedOutputs;
				for (const auto& Expression : Collection.Expressions)
				{
					if (!Live.contains(Expression.Get()))
					{
						Seeds.push_back(Expression.Get());
						ChangedOutputs.push_back(Expression.Get());
						continue;
					}
					bool bInputChanged = false;
					VisitStoredMaterialInputs(*Expression, [&](const FMaterialExpressionInput& Input) {
						bInputChanged |= Links.contains(&Input);
						if (!Numerics.empty())
							bInputChanged |= Numerics.contains(FindMaterialNumericInput(*Expression, Input));
					});
					if (bInputChanged) Seeds.push_back(Expression.Get());
				}

				FInferredNumericEdits Inferred;
				uint32 Count = 0;
				if (!Seeds.empty())
					InferNumericTypes(Collection.Expressions, Count, Seeds, ChangedOutputs,
						[&](DMaterialExpression& Expression, auto& Field, auto Value) {
							if (Live.contains(&Expression)) Inferred.Add(Expression, Field, std::move(Value));
							else Field = std::move(Value);
						}, Links, Numerics, {}, Outputs);
				return Inferred;
			}

			struct FGraphStructureWrite
			{
				std::vector<DObject*> Objects;
				std::vector<DMaterialExpression*> Removed;
				bool bMembershipChanged = false;
				bool bRootChanged = false;

				auto Apply(DObject& Owner, FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root) const -> void
				{
					if (bRootChanged) require(FMaterialExpressionEditing::SetRootInputs(*Cast<DMaterial>(&Owner), std::move(*Root)));
					if (!bMembershipChanged) return;
					FMaterialExpressionEditing::GetExpressions(Owner).swap(Collection.Expressions);
					for (auto* Node : Removed) Node->MarkAsGarbage();
					FMaterialExpressionEditing::ReconcileOwnership(Owner, {});
				}
			};

			auto PrepareStructureWrite(DObject& Owner, const FMaterialExpressionCollection& Collection, const FMaterialRootInputs* Root,
				FProposedLinks& Links, FProposedCallOutputs& CallOutputs, FProposedNumerics& Numerics,
				const FInferredNumericEdits& Inferred) -> FGraphStructureWrite
			{
				FGraphStructureWrite Write;
				const auto& Current = FMaterialExpressionEditing::GetExpressions(Owner);
				Write.bMembershipChanged = Collection.Expressions != Current;
				Write.bRootChanged = Root && *Root != Cast<DMaterial>(&Owner)->CaptureRootInputs();
				if (Write.bMembershipChanged || Write.bRootChanged) Write.Objects.push_back(&Owner);
				Write.Objects.insert(Write.Objects.end(), Inferred.Objects.begin(), Inferred.Objects.end());
				std::unordered_set<DMaterialExpression*> Live;
				for (const auto& Expression : Current) Live.insert(Expression.Get());
				std::erase_if(Links, [](const auto& Entry) { return *Entry.first == Entry.second; });
				std::erase_if(Numerics, [](const auto& Entry) { return *Entry.first == Entry.second; });
				for (const auto& Expression : Collection.Expressions)
				{
					const bool bRetained = Live.erase(Expression.Get()) != 0;
					// Detached additions are stored through the owner's membership record.
					if (!bRetained || (Links.empty() && Numerics.empty())) continue;
					VisitStoredMaterialInputs(*Expression, [&](const FMaterialExpressionInput& Input) {
						if (Links.contains(&Input)
							|| (!Numerics.empty() && Numerics.contains(FindMaterialNumericInput(*Expression, Input))))
							Write.Objects.push_back(Expression.Get());
					});
				}
				for (const auto& Expression : Current)
					if (Live.contains(Expression.Get())) Write.Removed.push_back(Expression.Get());
				Write.Objects.insert(Write.Objects.end(), Write.Removed.begin(), Write.Removed.end());
				CollectCallOutputEdits(CallOutputs, Write.Objects);
				return Write;
			}

			auto WriteExpressionInputs(FProposedLinks& Links, FProposedNumerics& Numerics) -> void
			{
				for (auto& [Slot, Input] : Numerics) *const_cast<FMaterialNumericInput*>(Slot) = std::move(Input);
				for (auto& [Slot, Link] : Links) *const_cast<FMaterialExpressionInput*>(Slot) = std::move(Link);
			}

			auto ValidateStructuralEdit(DObject& Owner, const FMaterialExpressionCollection& Collection,
				const FMaterialRootInputs* Root, const FProposedLinks& Links,
				const FMaterialParameterDefinition* SharedParameter = nullptr) -> FMaterialGraphCommandResult
			{
				if (!Root)
					if (const auto* Material = Cast<DMaterial>(&Owner))
					{
						const auto CurrentRoot = Material->CaptureRootInputs();
						return ValidateProposedGraph(Owner, Collection, Links, &CurrentRoot, SharedParameter);
					}
				return ValidateProposedGraph(Owner, Collection, Links, Root, SharedParameter);
			}
		}

		auto CommitGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
			FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root, FProposedLinks Links,
			FProposedCallOutputs CallOutputs, FProposedNumerics Numerics) -> FMaterialGraphCommandResult
		{
			if (auto Valid = ValidateStructuralEdit(Owner, Collection, Root, Links); !Valid) return Valid;
			auto Inferred = InferStructuralEdit(Owner, Collection, Links, Numerics, CallOutputs);
			auto Write = PrepareStructureWrite(Owner, Collection, Root, Links, CallOutputs, Numerics, Inferred);
			if (Write.Objects.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
			return RecordGraphEdit(Owner, Transactions, std::move(Description), Write.Objects, [&] {
				WriteExpressionInputs(Links, Numerics);
				WriteCallOutputs(CallOutputs);
				Inferred.Write();
				Write.Apply(Owner, Collection, Root);
			});
		}

		auto EditExpressionInputs(DObject& Owner, DTransactor* Transactions, std::string Description,
			std::vector<DMaterialExpression*> Objects, const std::function<void()>& Apply,
			const FProposedLinks& Links, const FProposedNumerics& Numerics,
			std::map<DMaterialExpression*, EMaterialProgramValueType> KnownOutputs, uint32* InferredCount,
			FProposedCallOutputs CallOutputs) -> FMaterialGraphCommandResult
		{
			if (auto Valid = ValidateGraphEdit(Owner, Links); !Valid) return Valid;
			FInferredNumericEdits Inferred;
			uint32 Count = 0;
			InferNumericTypes(FMaterialExpressionEditing::GetExpressions(Owner), Count, Objects, {},
				[&](DMaterialExpression& Object, auto& Field, auto Value) { Inferred.Add(Object, Field, std::move(Value)); },
				Links, Numerics, std::move(KnownOutputs), CallOutputs);
			if (InferredCount) *InferredCount = Count;
			std::vector<DObject*> Participants(Objects.begin(), Objects.end());
			Participants.insert(Participants.end(), Inferred.Objects.begin(), Inferred.Objects.end());
			CollectCallOutputEdits(CallOutputs, Participants);
			return RecordGraphEdit(Owner, Transactions, std::move(Description), Participants, [&] {
				WriteCallOutputs(CallOutputs);
				Apply();
				Inferred.Write();
			});
		}

		auto CommitExpressionReplacement(DObject& Owner, DMaterialExpression& Replacement,
			DTransactor* Transactions) -> FMaterialGraphCommandResult
		{
			FMaterialExpressionCollection Candidate{FMaterialExpressionEditing::GetExpressions(Owner)};
			auto* Existing = FindMaterialExpression(Candidate.Expressions, Replacement.Id);
			if (!Existing) return RejectCommand("The expression no longer exists.");
			std::vector<DMaterialExpression*> Peers;
			std::optional<FMaterialParameterDefinition> Shared;
			if (auto* Parameter = Cast<DMaterialExpressionParameter>(&Replacement))
			{
				if (auto Valid = ResolveParameterExpression(Owner, Candidate.Expressions, *Parameter,
					Cast<DMaterialExpressionParameter>(Existing), &Peers); !Valid) return Valid;
				if (!Peers.empty()) Shared = Parameter->GetParameterDefinition();
			}
			std::optional<FMaterialRootInputs> Root;
			if (const auto* Material = Cast<DMaterial>(&Owner)) Root = Material->CaptureRootInputs();
			FProposedLinks Links;
			PrepareExpressionReplacement(Candidate, Root ? &*Root : nullptr, Links, *Existing, Replacement);
			if (auto Valid = ValidateStructuralEdit(Owner, Candidate, Root ? &*Root : nullptr, Links, Shared ? &*Shared : nullptr); !Valid)
				return Valid;

			auto Inferred = InferStructuralEdit(Owner, Candidate, Links, {}, {});
			std::vector<std::pair<FProperty*, uint32>> Fields;
			if (Existing->GetClass() == Replacement.GetClass())
			{
				// Keep the original object and use the draft only for prospective values.
				const auto RestoreIdentity = [&](FMaterialExpressionInput& Input) {
					if (Input.Expression.Get() == &Replacement) Input.Expression = Existing;
				};
				VisitStoredMaterialInputs(Replacement, [&](FMaterialExpressionInput& Input) {
					if (const auto Proposed = Links.find(&Input); Proposed != Links.end())
					{
						Input = Proposed->second;
						Links.erase(Proposed);
					}
					RestoreIdentity(Input);
				});
				for (auto& [Slot, Link] : Links) RestoreIdentity(Link);
				if (Root)
				{
					RestoreIdentity(Root->Attributes);
					for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
						RestoreIdentity(Root->Get(Field.Property).Connection);
				}
				bool bCanCopy = true;
				Replacement.GetClass()->ForEachProperty([&](FProperty* Field) {
					if (Field->HasAnyPropertyFlags(EPropertyFlags::Transient) || Field->NamePrivate == FName("EditorLayout")) return;
					for (uint32 Index = 0; Index < Field->GetArrayDim(); ++Index)
						if (!ArePropertyValuesIdentical(Field, &Replacement, Index, Existing, Index))
						{
							bCanCopy &= Field->CanCopyAssignValue();
							Fields.emplace_back(Field, Index);
						}
				});
				if (!bCanCopy) return RejectCommand("Unable to update the expression properties.");
				*std::ranges::find(Candidate.Expressions, &Replacement, [](const auto& Expression) { return Expression.Get(); }) = Existing;
			}

			FProposedCallOutputs Outputs;
			FProposedNumerics Numerics;
			auto Write = PrepareStructureWrite(Owner, Candidate, Root ? &*Root : nullptr, Links, Outputs, Numerics, Inferred);
			if (!Fields.empty()) Write.Objects.push_back(Existing);
			Write.Objects.insert(Write.Objects.end(), Peers.begin(), Peers.end());
			if (Write.Objects.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
			return RecordGraphEdit(Owner, Transactions, "Replace Graph Expression", Write.Objects, [&] {
				for (const auto& [Field, Index] : Fields)
					require(Field->CopyAssignValue(Field->GetValuePtr(Existing, Index), Field->GetValuePtr(&Replacement, Index)));
				WriteExpressionInputs(Links, Numerics);
				for (auto* Peer : Peers) require(Cast<DMaterialExpressionParameter>(Peer)->SetParameterDefinition(*Shared));
				Inferred.Write();
				Write.Apply(Owner, Candidate, Root ? &*Root : nullptr);
			});
		}
	}

	auto GraphEditInternals::RemoveGraphNodes(DObject* Owner, std::span<const FGuid> NodeIds,
		bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		return WithGraphRecording(bShouldTransact, [&](DTransactor* Transactions) -> FMaterialGraphCommandResult {
			if (NodeIds.size() > MaterialProgramMaxNodeCount) return RejectCommand("The removal selection exceeds the graph bound.");
			if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
			FMaterialExpressionCollection Candidate{FMaterialExpressionEditing::GetExpressions(*Owner)};
			const std::unordered_set<FGuid> Selection(NodeIds.begin(), NodeIds.end());
			std::unordered_set<DMaterialExpression*> Removed;
			std::erase_if(Candidate.Expressions, [&](const auto& Expression) {
				if (!Selection.contains(Expression->Id)) return false;
				Removed.insert(Expression.Get());
				return true;
			});
			if (Removed.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
			std::optional<FMaterialRootInputs> Root;
			if (const auto* Material = Cast<DMaterial>(Owner))
			{
				Root = Material->CaptureRootInputs();
				const auto Clear = [&](FMaterialExpressionInput& Input) { if (Removed.contains(Input.Expression.Get())) Input = {}; };
				Clear(Root->Attributes);
				for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) Clear(Root->Get(Field.Property).Connection);
			}
			FProposedLinks Links;
			for (const auto& Expression : Candidate.Expressions)
				VisitStoredMaterialInputs(*Expression, [&](const FMaterialExpressionInput& Input) {
					if (Removed.contains(Input.Expression.Get())) Links.emplace(&Input, FMaterialExpressionInput{});
				});
			return CommitGraphEdit(*Owner, Transactions, "Remove Graph Nodes", Candidate, Root ? &*Root : nullptr, std::move(Links));
		});
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

	auto DMaterialGraph::RemoveNodes(std::span<const FGuid> NodeIds, bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		if (std::ranges::contains(NodeIds, RootViewId)) return GraphEditInternals::RejectCommand("The material root cannot be removed.");
		return GraphEditInternals::RemoveGraphNodes(GetMaterialOrFunction(), NodeIds, bShouldTransact);
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
			return Current == Pin && Graph && Graph->FindNode(Node->GetView().Node.Id) == Node
				&& (Node->GetMaterialExpression() || Graph->GetRootNode() == Node) ? Graph : nullptr;
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
		bool Replace, bool bShouldTransact, std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		const auto Response = CanCreateConnection(A, B);
		if (!Response.Allows(Replace))
		{
			if (OutError) *OutError = Response.Reason;
			return false;
		}
		const auto* Input = A->Direction == EMaterialGraphPinDirection::Input ? A : B;
		return PinGraph(Input)->LinkMaterialExpressionsFromGraph(*Input, Input == A ? B : A, Replace, bShouldTransact, OutError);
	}

	auto FMaterialGraphSchema::BreakPinLinks(const FMaterialGraphPin* Pin, bool bShouldTransact, std::string* OutError) const -> bool
	{
		auto* Graph = PinGraph(Pin);
		if (!Graph) return GraphEditInternals::FailWithError(OutError, "The pin is unavailable.");
		if (Pin->Direction == EMaterialGraphPinDirection::Output) return Graph->BreakOutputLinks(*Pin, bShouldTransact, OutError);
		return Graph->LinkMaterialExpressionsFromGraph(*Pin, nullptr, true, bShouldTransact, OutError);
	}

	auto DMaterialGraphNode::CanSetAttributeInput(EMaterialProperty Property, const FMaterialGraphPin* Output) const -> FMaterialGraphConnectionResponse
	{
		auto* Graph = PinGraph(Output);
		if (!Graph || Graph != GetOuter() || Output->Direction != EMaterialGraphPinDirection::Output
			|| !Cast<DMaterialExpressionSetMaterialAttributes>(GetMaterialExpression()) || !FMaterialAttributeDefinitionMap::Find(Property))
			return {EMaterialGraphConnectionResponse::Disallow, "The attribute input or source output is unavailable."};
		if (const auto* Input = FindInputPin(static_cast<uint32>(Property) + 1)) return Graph->GetSchema().CanCreateConnection(Output, Input);
		const auto SourceType = Output->GetOwningNode()->GetView().Outputs[Output->DisplayIndex].Type;
		const auto Type = GetMaterialSurfaceOutputType(Property);
		const bool Compatible = SourceType == Type || (MaterialInputAllowsValidCast(EMaterialProgramOpcode::MakeMaterialAttributes, uint32(Property))
			&& FMaterialGraphSchema::AcceptsPort(Type, SourceType));
		return CheckConnection(*Output, *this, Compatible, nullptr);
	}

	auto DMaterialGraphNode::SetAttributeInput(EMaterialProperty Property, const FMaterialGraphPin& Output,
		bool bShouldTransact, std::string* OutError) -> bool
	{
		using namespace GraphEditInternals;
		const auto Response = CanSetAttributeInput(Property, &Output);
		if (!Response.Allows(true)) return FailWithError(OutError, Response.Reason);
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (const auto* Input = FindInputPin(static_cast<uint32>(Property) + 1))
			return Graph->GetSchema().TryCreateConnection(&Output, Input, true, bShouldTransact, OutError);

		return WithGraphRecording(bShouldTransact, OutError, [&](DTransactor* Transactions) -> bool {
			auto* Target = Cast<DMaterialExpressionSetMaterialAttributes>(GetMaterialExpression());
			auto* Source = Output.GetOwningNode();
			const auto& Pin = Source->GetView().Outputs[Output.DisplayIndex];
			auto& Owner = *Target->GetOuter();
			FProposedCallOutputs Outputs;
			const auto Link = ResolveGraphOutput(Source->GetMaterialExpression(), Pin.OutputIndex, Pin.PortId, Outputs);
			if (Link.OutputIndex < 0) return FailWithError(OutError, "The source output is unavailable.");
			auto Bindings = Target->Attributes;
			Bindings.emplace_back(Property, Link);
			const FMaterialExpressionCollection Collection{FMaterialExpressionEditing::GetExpressions(Owner)};
			if (auto Valid = ValidateStructuralEdit(Owner, Collection, nullptr, {}); !Valid)
				return ConnectionCommitChanged(Valid, OutError);
			std::vector<DObject*> Objects{Target};
			CollectCallOutputEdits(Outputs, Objects);
			return ConnectionCommitChanged(RecordGraphEdit(Owner, Transactions, "Set Material Attribute Input", Objects, [&] {
				WriteCallOutputs(Outputs);
				Target->Attributes.swap(Bindings);
			}), OutError);
		});
	}

	auto DMaterialGraphNode::ClearAttributeInput(EMaterialProperty Property, bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto* Target = Cast<DMaterialExpressionSetMaterialAttributes>(GetMaterialExpression());
		if (!Target || !FMaterialAttributeDefinitionMap::Find(Property)) return RejectCommand("The attribute input is unavailable.");
		const auto It = std::ranges::find(Target->Attributes, Property, &FMaterialExpressionAttributeBinding::GetProperty);
		if (It == Target->Attributes.end()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return WithGraphRecording(bShouldTransact, [&](DTransactor* Transactions) -> FMaterialGraphCommandResult {
			auto& Owner = *Target->GetOuter();
			if (auto Valid = ValidateGraphEdit(Owner, {{&It->Input, {}}}); !Valid) return Valid;
			return EditExpressions(Owner, Transactions, "Clear Material Attribute Input",
				std::array<DMaterialExpression*, 1>{Target}, [&] { Target->Attributes.erase(It); });
		});
	}

	auto DMaterialGraph::BreakOutputLinks(const FMaterialGraphPin& Output, bool bShouldTransact, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		using namespace GraphEditInternals;
		auto* Owner = GetMaterialOrFunction();
		return WithGraphRecording(bShouldTransact, OutError, [&](DTransactor* Transactions) -> bool {
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
				auto Root = Material->CaptureRootInputs();
				bool RootChanged = false;
				const auto Clear = [&](EMaterialProperty Property) {
					auto* Link = Root.GetExpressionInputForProperty(Property);
					if (Link->Expression.Get() == Upstream && Link->OutputIndex == Output.SourceIndex)
					{
						*Link = {};
						RootChanged = true;
					}
				};
				Clear(EMaterialProperty::MP_MaterialAttributes);
				for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) Clear(Field.Property);
				if (RootChanged)
				{
					FProposedLinks Links;
					for (const auto& Consumer : Consumers) Links.emplace(Consumer.Input, FMaterialExpressionInput{});
					auto Candidate = Material->GetExpressionCollection();
					return ConnectionCommitChanged(CommitGraphEdit(*Material, Transactions, "Break Output Pin Links", Candidate, &Root, std::move(Links)), OutError);
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
			const auto Result = EditExpressionInputs(*Owner, Transactions, "Break Output Pin Links", std::move(Objects),
				[&] { for (const auto& Consumer : Consumers) *Consumer.Input = {}; }, Links);
			return ConnectionCommitChanged(Result, OutError);
		});
	}

	auto DMaterialGraphNode::GetMaterialExpression() const -> DMaterialExpression*
	{
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Graph->FindNode(View.Node.Id) != this) return nullptr;
		auto* Expression = MaterialExpression.Get();
		auto* Owner = Graph->GetMaterialOrFunction();
		if (!Owner || !Expression || Expression->GetOuter() != Owner) return nullptr;
		const auto& Members = FMaterialExpressionEditing::GetExpressions(*Owner);
		return std::ranges::contains(Members, Expression, [](const auto& Member) { return Member.Get(); }) ? Expression : nullptr;
	}

	auto DMaterialGraphNode::ResolveInputGraph(const FMaterialGraphPin* Pin) const -> DMaterialGraph*
	{
		return Pin && Pin->Direction == EMaterialGraphPinDirection::Input && Pin->GetOwningNode() == this ? PinGraph(Pin) : nullptr;
	}

	auto DMaterialGraphNode::PinDefaultValueChanged(const FMaterialGraphPin* Pin, FMaterialInputDefault Value,
		bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto* Graph = ResolveInputGraph(Pin);
		if (!Graph) return RejectCommand("The input pin is unavailable.");
		const auto& Input = View.Inputs[Pin->DisplayIndex];
		if (Input.MaterialProperty)
			return WriteInputDefault(Cast<DMaterial>(Graph->GetMaterialOrFunction()), *Input.MaterialProperty, std::move(Value), bShouldTransact);
		return WriteInputDefault(Graph->GetMaterialOrFunction(), GetMaterialExpression(), GetExpressionInputKey(Input), std::move(Value), bShouldTransact);
	}

	auto DMaterialGraphNode::SetPinUseConstant(const FMaterialGraphPin* Pin, bool Enabled,
		bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto* Graph = ResolveInputGraph(Pin);
		if (!Graph) return RejectCommand("The numeric input pin is unavailable.");
		const auto& Input = View.Inputs[Pin->DisplayIndex];
		if (Input.MaterialProperty)
			return SetInputConstantEnabled(Cast<DMaterial>(Graph->GetMaterialOrFunction()), *Input.MaterialProperty, Enabled, bShouldTransact);
		if (Input.PortId.IsValid()) return RejectCommand("The numeric input pin is unavailable.");
		return SetInputConstantEnabled(Graph->GetMaterialOrFunction(), GetMaterialExpression(), Input.InputIndex, Enabled, bShouldTransact);
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
			return FindExpressionOutputPin(*E, Input.OutputIndex).has_value();
		}

		auto WriteRootInput(DMaterial* Material, EMaterialProperty Property, DMaterialExpression* Source, int32 Index, FGuid Port,
			bool Replace, bool bShouldTransact, std::string* Error) -> bool
		{
			return WithGraphRecording(bShouldTransact, Error, [&](DTransactor* Transactions) -> bool {
				if (!Material) return FailWithError(Error, "Invalid material root address.");
				auto Root = Material->CaptureRootInputs();
				auto* Input = Root.GetExpressionInputForProperty(Property);
				if (!Input) return FailWithError(Error, "The material attribute pin is invalid.");
				FProposedCallOutputs Outputs;
				const auto Next = ResolveGraphOutput(Source, Port.IsValid() ? 0 : Index, Port, Outputs);
				if (Next.OutputIndex < 0) return FailWithError(Error, "The source output no longer exists.");
				if (!FMaterialGraphSchema::CanReplaceConnection(*Input, Next, Replace)) return FailWithError(Error, "The graph input is already connected.");
				const bool bRootChanged = *Input != Next;
				*Input = Next;
				if (auto Valid = ValidateProposedGraph(*Material, Material->GetExpressionCollection(), {}, &Root, nullptr); !Valid)
					return ConnectionCommitChanged(Valid, Error);
				std::vector<DObject*> Objects;
				if (bRootChanged) Objects.push_back(Material);
				CollectCallOutputEdits(Outputs, Objects);
				if (Objects.empty()) return ConnectionCommitChanged({.Status = EMaterialGraphCommandStatus::NoChange}, Error);
				return ConnectionCommitChanged(RecordGraphEdit(*Material, Transactions, "Connect Material Input", Objects, [&] {
					WriteCallOutputs(Outputs);
					if (bRootChanged) require(FMaterialExpressionEditing::SetRootInputs(*Material, std::move(Root)));
				}), Error);
			});
		}

		auto WriteInput(DObject* Owner, DMaterialExpression& Node, FExpressionInputKey Key,
			DMaterialExpression* Upstream, int32 SourceIndex, FGuid UnboundPort, bool bReplaceExisting, bool bShouldTransact, std::string* OutError) -> bool
		{
			if (OutError) OutError->clear();
			return WithGraphRecording(bShouldTransact, OutError, [&](DTransactor* Transactions) -> bool {
				if (!Owner) return GraphEditInternals::FailWithError(OutError, "The material graph owner is no longer available.");
				FMaterialExpressionInput* Target = nullptr;
				auto* Call = Cast<DMaterialExpressionFunctionCall>(&Node);
				const FMaterialFunctionPort* Port = nullptr;
				if (const auto* PortId = std::get_if<FGuid>(&Key))
				{
					if (!Call || !PortId->IsValid()) return GraphEditInternals::FailWithError(OutError, "The function call is unavailable.");
					const auto Info = Call->FindInputInfo(*PortId);
					Port = Info.FunctionPort;
					if (!Port) return GraphEditInternals::FailWithError(OutError, "The function input port no longer exists.");
					const auto Binding = Info.StoredIndex.value_or(-1);
					if (Binding >= 0) Target = &Call->FunctionInputs[Binding].Input;
				}
				else
				{
					if (Call) return GraphEditInternals::FailWithError(OutError, "The input pin key is invalid.");
					const uint32 Index = std::get<uint32>(Key);
					if (std::in_range<int32>(Index)) Target = Node.GetInput(static_cast<int32>(Index));
					if (!Target) return GraphEditInternals::FailWithError(OutError, "The graph input no longer exists.");
				}
				FProposedCallOutputs Outputs;
				const auto Connection = ResolveGraphOutput(Upstream, SourceIndex, UnboundPort, Outputs);
				if (Connection.OutputIndex < 0)
					return FailWithError(OutError, "The source output no longer exists.");
				const auto Previous = Target ? *Target : FMaterialExpressionInput{};
				if (!FMaterialGraphSchema::CanReplaceConnection(Previous, Connection, bReplaceExisting))
					return GraphEditInternals::FailWithError(OutError, "The graph input is already connected.");
				if (Previous == Connection) return false;
				if (Target)
					return ConnectionCommitChanged(EditExpressionInputs(*Owner, Transactions, "Connect Graph Input", {&Node},
						[&] { *Target = Connection; }, {{Target, Connection}}, {}, {}, nullptr, std::move(Outputs)), OutError);

				const auto& Members = FMaterialExpressionEditing::GetExpressions(*Owner);
				if (Upstream && !std::ranges::contains(Members, Upstream, [](const auto& Member) { return Member.Get(); }))
					return FailWithError(OutError, "The source expression is unavailable.");
				if (auto Valid = ValidateGraphEdit(*Owner); !Valid) return ConnectionCommitChanged(Valid, OutError);
				auto Bindings = Call->FunctionInputs;
				Bindings.push_back({Port->Id, Port->Type, Connection});
				std::vector<DObject*> Objects{Call};
				CollectCallOutputEdits(Outputs, Objects);
				return ConnectionCommitChanged(RecordGraphEdit(*Owner, Transactions, "Connect Graph Input", Objects, [&] {
					WriteCallOutputs(Outputs);
					Call->FunctionInputs.swap(Bindings);
				}), OutError);
			});
		}

	}

	auto DMaterialGraph::LinkMaterialExpressionsFromGraph(const FMaterialGraphPin& Input, const FMaterialGraphPin* Output,
		bool Replace, bool bShouldTransact, std::string* OutError) -> bool
	{
		auto* Node = Input.GetOwningNode();
		const auto& Target = Node->GetView().Inputs[Input.DisplayIndex];
		auto* Expression = Node->GetMaterialExpression();
		if (Target.MaterialProperty)
		{
			const auto* Source = Output ? Output->GetOwningNode() : nullptr;
			return WriteRootInput(Cast<DMaterial>(GetMaterialOrFunction()), *Target.MaterialProperty, Source ? Source->GetMaterialExpression() : nullptr,
				Output ? Output->SourceIndex : 0, Output ? Source->GetView().Outputs[Output->DisplayIndex].PortId : FGuid{}, Replace, bShouldTransact, OutError);
		}
		if (!Expression) return GraphEditInternals::FailWithError(OutError, "The graph input is unavailable.");
		if (!Output) return WriteInput(GetMaterialOrFunction(), *Expression, GraphEditInternals::GetExpressionInputKey(Target), nullptr, 0, {}, Replace, bShouldTransact, OutError);
		auto* Source = Output->GetOwningNode();
		auto* Upstream = Source->GetMaterialExpression();
		if (!Upstream) return GraphEditInternals::FailWithError(OutError, "The source output no longer exists.");
		const auto PortId = Source->GetView().Outputs[Output->DisplayIndex].PortId;
		const auto Index = Output->SourceIndex;
		const auto Pin = GraphEditInternals::FindExpressionOutputPin(*Upstream, Index, PortId);
		if (!Pin || Pin->Output.PortId != PortId || Pin->SourceIndex != Index)
			return GraphEditInternals::FailWithError(OutError, "The source output no longer exists or its binding has changed.");
		return WriteInput(GetMaterialOrFunction(), *Expression, GraphEditInternals::GetExpressionInputKey(Target), Upstream, Index, Index < 0 ? PortId : FGuid{}, Replace, bShouldTransact, OutError);
	}

	// Low-level repair writes may address input slots without a projected graph pin.
	auto GraphEditInternals::WriteExpressionConnection(DObject* Owner, DMaterialExpression* Target, FExpressionInputKey Key,
		FMaterialExpressionInput Source, bool Replace, bool bShouldTransact, std::string* OutError, FGuid SourcePort) -> bool
	{
		if (!Owner || !Target) return FailWithError(OutError, "The graph input no longer exists.");
		const auto& Members = FMaterialExpressionEditing::GetExpressions(*Owner);
		if (!std::ranges::contains(Members, Target, [](const auto& Member) { return Member.Get(); }))
			return FailWithError(OutError, "The graph input no longer exists.");
		auto* Upstream = Source.Expression.Get();
		if (SourcePort.IsValid())
		{
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Upstream))
			{
				const auto Pin = FindExpressionOutputPin(*Call, -1, SourcePort);
				if (!Pin) return FailWithError(OutError, "The source output no longer exists.");
				Source.OutputIndex = Pin->SourceIndex;
			}
			else Source = FMaterialExpressionInput(Upstream, 0, SourcePort);
		}
		const auto Unbound = Cast<DMaterialExpressionFunctionCall>(Upstream) && Source.OutputIndex < 0 ? SourcePort : FGuid{};
		if (Unbound.IsValid() ? !std::ranges::contains(Members, Upstream, [](const auto& Member) { return Member.Get(); })
			: !IsSourceAvailable(Members, Source))
			return FailWithError(OutError, "The source output no longer exists.");
		return WriteInput(Owner, *Target, Key, Upstream, Source.OutputIndex, Unbound, Replace, bShouldTransact, OutError);
	}

	auto GraphEditInternals::WriteRootConnection(DMaterial* Material, EMaterialProperty Property,
		FMaterialExpressionInput Source, bool Replace, bool bShouldTransact, std::string* OutError, FGuid SourcePort) -> bool
	{
		if (!Material) return FailWithError(OutError, "The material is unavailable.");
		const auto& Members = FMaterialExpressionEditing::GetExpressions(*Material);
		if (SourcePort.IsValid())
		{
			if (!std::ranges::contains(Members, Source.Expression.Get(), [](const auto& Member) { return Member.Get(); }))
				return FailWithError(OutError, "The source output no longer exists.");
		}
		else if (!IsSourceAvailable(Members, Source)) return FailWithError(OutError, "The source output no longer exists.");
		return WriteRootInput(Material, Property, Source.Expression.Get(), Source.OutputIndex, SourcePort, Replace, bShouldTransact, OutError);
	}

	auto DMaterialGraph::FindOutputGraphPin(const FMaterialProgramLink& Link) const -> const FMaterialGraphPin*
	{
		const auto* Node = FindNode(Link.SourceNodeId);
		if (!Node) return nullptr;
		const auto& Outputs = Node->GetView().Outputs;
		const auto Pin = std::ranges::find_if(Outputs, [&](const auto& Output) {
			return Output.PortId == Link.SourceOutputId && (Output.PortId.IsValid() || Output.OutputIndex == Link.SourceOutputIndex);
		});
		return Node->GetOutputPin(static_cast<int32>(Pin - Outputs.begin()));
	}

	auto DMaterialGraph::FindOutputPin(const FMaterialProgramLink& Link) const -> const FMaterialGraphOutputPinView*
	{
		const auto* Pin = FindOutputGraphPin(Link);
		return Pin ? &Pin->GetOwningNode()->GetView().Outputs[Pin->DisplayIndex] : nullptr;
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

	auto DMaterialGraph::RefreshNodes(std::optional<std::span<const FGuid>> Selected) -> void
	{
		auto* Asset = GetMaterialOrFunction();
		const auto* Material = Cast<DMaterial>(Asset);
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(*Asset);
		std::unordered_map<FGuid, DMaterialGraphNode*> Existing;
		for (const auto& Node : Nodes) Existing.emplace(Node->GetView().Node.Id, Node.Get());
		std::unordered_set<FGuid> Selection;
		if (Selected) Selection.insert(Selected->begin(), Selected->end());
		std::vector<TObjectPtr<DMaterialGraphNode>> Updated;
		const auto FindOrAddNode = [&](FGuid Id, DClass* Type) {
			const auto It = Existing.find(Id);
			auto* Node = It != Existing.end() ? It->second
				: NewObject<DMaterialGraphNode>(Type, this, NAME_None, EObjectFlags::Transient);
			if (!Selected) Updated.emplace_back(Node);
			else if (It == Existing.end()) Nodes.emplace_back(Node);
			return Node;
		};
		for (size_t Ordinal = 0; Ordinal < Expressions.size(); ++Ordinal)
		{
			auto* Expression = Expressions[Ordinal].Get();
			if (Expression && (!Selected || Selection.contains(Expression->Id)))
				FindOrAddNode(Expression->Id, DMaterialGraphNode::StaticClass())->Refresh(Expression, Ordinal);
		}
		if (Material && (!Selected || Selection.contains(RootViewId)))
			Cast<DMaterialGraphNode_Root>(FindOrAddNode(RootViewId, DMaterialGraphNode_Root::StaticClass()))->Refresh(*Material, RootViewId);
		if (!Selected)
		{
			// Detached nodes may still be held by an editor client.
			for (const auto& Node : Nodes)
				Node->ClearPinLinks();
			Nodes = std::move(Updated);
		}
		std::ranges::sort(Nodes, {}, [](const auto& Node) { return Node->GetView().Node.Id; });
		Outputs = Material ? ReadGraphOutputs(*Material) : FMaterialAttributeValues{};
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
