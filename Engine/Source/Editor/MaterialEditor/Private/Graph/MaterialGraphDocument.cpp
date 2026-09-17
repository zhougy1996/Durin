#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionBuild.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphEditSession.h"
#include "DObject/Package.h"
#include "MaterialGraphValueTypes.h"
#include <cmath>

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;
	namespace GraphEditInternals
	{
		// Update inferred widths on live objects, recording each participant before
		// writing. Unresolvable types remain compiler diagnostics during editing.
		template<class TState>
		auto AdaptNumericTypesImpl(TState& State, std::span<const FGuid> ChangedNodes, std::span<const FGuid> ChangedOutputNodes) -> bool
		{
			using Type = EMaterialProgramValueType;
			const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
			std::map<FGuid, DMaterialExpression*> Expressions;
			std::map<FGuid, Type> Resolved;
			std::set<FGuid> Active;
			for (const auto& E : State.Expressions) Expressions.emplace(E->Id, E.Get());
			// Build reverse edges once for this edit. Only the downstream closure is
			// eligible for inference; unrelated objects retain their authored state.
			std::map<FGuid, std::vector<FGuid>> Consumers;
			for (const auto& E : State.Expressions)
				VisitMaterialExpressionInputs(*E, [&](uint32, const FMaterialExpressionInput& Input) {
					if (Input.ExpressionId.IsValid()) Consumers[Input.ExpressionId].push_back(E->Id);
				});
			std::set<FGuid> Seeds(ChangedNodes.begin(), ChangedNodes.end()), Affected = Seeds, ChangedTypes;
			const std::set<FGuid> ForcedOutputs(ChangedOutputNodes.begin(), ChangedOutputNodes.end());
			std::vector<FGuid> Pending(ChangedNodes.begin(), ChangedNodes.end());
			for (size_t I = 0; I < Pending.size(); ++I)
				for (const auto& Consumer : Consumers[Pending[I]])
					if (Affected.insert(Consumer).second) Pending.push_back(Consumer);
			bool bValid = true;
			std::function<Type(const FMaterialExpressionInput&)> Resolve;
			Resolve = [&](const FMaterialExpressionInput& Input) -> Type {
				const auto Found = Expressions.find(Input.ExpressionId);
				if (Found == Expressions.end()) { bValid = false; return Type::Float; }
				auto* E = Found->second;
				if (Seeds.contains(E->Id) && !Resolved.contains(E->Id)) ChangedTypes.insert(E->Id);
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(E))
				{
					const auto Port = std::ranges::find(Call->Outputs, Input.OutputId, &FMaterialFunctionOutputBinding::OutputId);
					if (Port != Call->Outputs.end()) return Port->ExpectedType;
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
				if (Opcode == EMaterialProgramOpcode::GetSurfaceAttributes && Input.OutputIndex < 8)
					return GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Input.OutputIndex));
				if (Resolved.contains(E->Id)) return Resolved.at(E->Id);
				if (!Active.insert(E->Id).second) { bValid = false; return Type::Float; }
				auto* Property = E->GetClass()->FindPropertyByName("ResultType");
				auto Result = Property ? *static_cast<Type*>(Property->GetValuePtr(E)) : Shape->ResultType;
				if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(E))
					Result = static_cast<Type>(Swizzle->Components.size() - 1);
				const auto PreviousType = Result;
				if (!Affected.contains(E->Id)) { Active.erase(E->Id); return Result; }
				bool bInputChanged = false;
				VisitMaterialExpressionInputs(*E, [&](uint32, const FMaterialExpressionInput& Operand) {
					if (!Operand.ExpressionId.IsValid()) return;
					Resolve(Operand);
					bInputChanged |= ChangedTypes.contains(Operand.ExpressionId);
				});
				if (!Seeds.contains(E->Id) && !bInputChanged)
				{
					Active.erase(E->Id); Resolved.emplace(E->Id, Result); return Result;
				}
				if (IsMaterialAdaptiveNumeric(Opcode) || Opcode == EMaterialProgramOpcode::AppendVector) ++State.InferredNumericNodes;
				if (auto* Append = Cast<DMaterialExpressionAppendVector>(E))
				{
					const auto Width = [&](const FMaterialExpressionInput& Operand, const std::vector<float>& Default) -> size_t {
						if (!Operand.ExpressionId.IsValid()) return Default.size();
						const auto Value = Resolve(Operand);
						return Value <= Type::Float4 ? static_cast<size_t>(Value) + 1 : 5;
					};
					const auto AWidth = Width(Append->A, Append->ADefault);
					const auto BWidth = Width(Append->B, Append->BDefault);
					if (!AWidth || !BWidth || AWidth + BWidth > 4) bValid = false;
					else
					{
						const auto Inferred = static_cast<Type>(AWidth + BWidth - 1);
						if (Append->ResultType != Inferred) { State.Modify(*Append); Append->ResultType = Inferred; }
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
						if (Operand.ExpressionId.IsValid()) Merge(Resolve(Operand));
						else E->GetClass()->ForEachProperty([&](FProperty* P) {
							if (P->GetValuePtr(E) != &Operand) return;
							auto* Default = E->GetClass()->FindPropertyByName(FName(P->NamePrivate.ToString() + "Default"));
							if (!Default) return;
							const auto& Values = *static_cast<std::vector<float>*>(Default->GetValuePtr(E));
							// Uniform defaults are width-independent; retain authored components otherwise.
							if (Values.size() > 1 && !std::ranges::all_of(Values, [&](float V) { return V == Values.front(); }))
								Merge(static_cast<Type>(Values.size() - 1));
						});
					});
					if (bOperand) Result = Inferred;
					if (!GetMaterialProgramNodeSignature(Opcode, Result)) bValid = false;
					if (Property && *static_cast<Type*>(Property->GetValuePtr(E)) != Result)
					{ State.Modify(*E); *static_cast<Type*>(Property->GetValuePtr(E)) = Result; }
					VisitMaterialExpressionInputs(*E, [&](uint32 Slot, FMaterialExpressionInput& Operand) {
						if (Opcode == EMaterialProgramOpcode::Lerp && Slot == 2) return;
						E->GetClass()->ForEachProperty([&](FProperty* P) {
							if (P->GetValuePtr(E) != &Operand) return;
							auto* Default = E->GetClass()->FindPropertyByName(FName(P->NamePrivate.ToString() + "Default"));
							if (!Default) return;
							auto& Values = *static_cast<std::vector<float>*>(Default->GetValuePtr(E));
							if (Values.size() > 1 && Values.size() != static_cast<size_t>(Result) + 1
								&& std::ranges::all_of(Values, [&](float V) { return V == Values.front(); })) { State.Modify(*E); Values.resize(1); }
						});
					});
				}
				if (Result != PreviousType) ChangedTypes.insert(E->Id);
				else if (!ForcedOutputs.contains(E->Id) && (IsMaterialAdaptiveNumeric(Opcode) || Opcode == EMaterialProgramOpcode::AppendVector))
					ChangedTypes.erase(E->Id);
				Active.erase(E->Id);
				Resolved.emplace(E->Id, Result);
				return Result;
			};
			for (const auto& E : State.Expressions)
			{
				const auto Shape = std::ranges::find(Catalog, E->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Affected.contains(E->Id) && Shape != Catalog.end() && (IsMaterialAdaptiveNumeric(Shape->Opcode) || Shape->Opcode == EMaterialProgramOpcode::AppendVector)) Resolve({E->Id});
			}
			return bValid;
		}

		auto AdaptNumericTypes(FGraphEditSession& State, std::span<const FGuid> ChangedNodes, std::span<const FGuid> ChangedOutputNodes) -> bool { return AdaptNumericTypesImpl(State, ChangedNodes, ChangedOutputNodes); }

		template<class TState>
		auto IsSourceAvailable(const TState& State, const FMaterialExpressionInput& Input) -> bool
		{
			if (!Input.ExpressionId.IsValid()) return true;
			const auto It = std::ranges::find(State.Expressions, Input.ExpressionId, [](auto& E) { return E->Id; });
			if (It == State.Expressions.end()) return false;
			const auto* E = It->Get();
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(E))
				return Call->Function.IsValid() && Input.OutputIndex == 0
					&& std::ranges::any_of(Call->Function->GetFunctionSignature().Outputs, [&](const auto& Port) { return Port.Id == Input.OutputId; });
			if (Input.OutputId.IsValid() || Cast<DMaterialExpressionMaterialOutput>(E) || Cast<DMaterialExpressionFunctionOutput>(E)) return false;
			if (Cast<DMaterialExpressionTextureSampleParameter2D>(E))
				return FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSampleParameter2D, Input.OutputIndex) != nullptr;
			if (Cast<DMaterialExpressionTextureSample2D>(E))
				return FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSample2D, Input.OutputIndex) != nullptr;
			if (const auto* Surface = Cast<DMaterialExpressionGetSurfaceAttributes>(E))
				return Input.OutputIndex < 8 && (Surface->AttributeMask & (1u << Input.OutputIndex));
			return Input.OutputIndex == 0;
		}

		template<class TState>
		auto FindCall(TState& State, const FGuid& Id) -> DMaterialExpressionFunctionCall*
		{
			for (auto& Expression : State.Expressions)
				if (Expression->Id == Id) return Cast<DMaterialExpressionFunctionCall>(Expression.Get());
			return nullptr;
		}
		template<class TState>
		auto IncludeCallOutput(TState& State, const FMaterialExpressionInput& Source) -> void
		{
			if (!Source.OutputId.IsValid()) return;
			auto* Call = FindCall(State, Source.ExpressionId);
			if (!Call || !Call->Function.IsValid()
				|| std::ranges::find(Call->Outputs, Source.OutputId, &FMaterialFunctionOutputBinding::OutputId) != Call->Outputs.end()) return;
			const auto& Ports = Call->Function->GetFunctionSignature().Outputs;
			if (const auto Port = std::ranges::find(Ports, Source.OutputId, &FMaterialFunctionPort::Id); Port != Ports.end())
				{ State.Modify(*Call); Call->Outputs.push_back({Port->Id, Port->Type}); }
		}

	}

	FMaterialGraphDocument::FMaterialGraphDocument(DObject& InOwner) : Owner(&InOwner) {}

	auto FMaterialGraphDocument::SetPort(bool bOutput, FMaterialFunctionPort Port,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return MakeRejected("Only function documents have an interface.");
		FGraphEditSession State(*Function);
		for (auto& Expression : State.Expressions)
		{
			FMaterialFunctionPort* Target = nullptr;
			if (bOutput)
			{
				if (auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) Target = &Terminal->Port;
			}
			else if (auto* Terminal = Cast<DMaterialExpressionFunctionInput>(Expression.Get())) Target = &Terminal->Port;
			if (!Target || Target->Id != Port.Id) continue;
			if (*Target == Port) return {.Status = EMaterialGraphCommandStatus::NoChange};
			State.Modify(*Expression);
			*Target = std::move(Port);
			return CommitGraphEdit(*Function, State, "Edit Function Port", Transactions);
		}
		return MakeRejected("The function port is unavailable.");
	}

	auto FMaterialGraphDocument::AddPort(bool bOutput, FMaterialFunctionPort Port,
		FMaterialProgramLink Source, int32 X, int32 Y, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return MakeRejected("Only function documents have an interface.");
		FGraphEditSession State(*Function);
		if (!Port.Id.IsValid()) Port.Id = FGuid::NewGuid();
		const auto NodeId = FGuid::NewGuid();
		if (bOutput)
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->Port = std::move(Port);
			Terminal->Source = {Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
			State.Expressions.emplace_back(Terminal);
		}
		else
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->Port = std::move(Port);
			State.Expressions.emplace_back(Terminal);
		}
		State.Presentation.Nodes.push_back({NodeId, X, Y});
		auto Result = CommitGraphEdit(*Function, State,
			bOutput ? "Add Function Output" : "Add Function Input", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {NodeId};
		return Result;
	}

	auto FMaterialGraphDocument::RemovePort(bool bOutput, const FGuid& PortId,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return MakeRejected("Only function documents have an interface.");
		FGraphEditSession State(*Function);
		const auto Removed = std::erase_if(State.Expressions, [&](const auto& Expression) {
			if (bOutput)
			{
				const auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression.Get());
				return Terminal && Terminal->Port.Id == PortId;
			}
			const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(Expression.Get());
			return Terminal && Terminal->Port.Id == PortId;
		});
		if (!Removed) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return CommitGraphEdit(*Function, State, "Remove Function Port", Transactions);
	}

	auto FMaterialGraphDocument::CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X, int32 Y,
		FMaterialExpressionInput FirstInput, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
		if (std::ranges::none_of(Catalog, [&](const auto& Expected) {
			return Entry.Opcode == Expected.Opcode && Entry.ResultType == Expected.ResultType
				&& Entry.ExpressionClass == Expected.ExpressionClass && Entry.AcceptedInputTypes == Expected.AcceptedInputTypes;
		})) return MakeRejected("The catalog expression shape is stale.");
		if (FirstInput.ExpressionId.IsValid() && Entry.AcceptedInputTypes.empty())
			return MakeRejected("This catalog expression has no input pin.");
		FGraphEditSession State(*Owner.Get());
		TStrongObjectPtr<DMaterialExpression> Expression(NewObject<DMaterialExpression>(Entry.ExpressionClass, nullptr, NAME_None));
		if (!Expression) return MakeRejected("The catalog expression class is unavailable.");
		Expression->Id = FGuid::NewGuid();
		if (auto* Property = Expression->GetClass()->FindPropertyByName("ResultType"))
			*static_cast<EMaterialProgramValueType*>(Property->GetValuePtr(Expression.Get())) = Entry.ResultType;
		if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()))
		{
			if (Entry.ResultType > EMaterialProgramValueType::Float4) return MakeRejected("The swizzle width is invalid.");
			Swizzle->Components.clear();
			for (uint8 Index = 0; Index <= static_cast<uint8>(Entry.ResultType); ++Index) Swizzle->Components.push_back(Index);
		}
		FGuid ParameterId;
		std::string DisplayName;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
		{
			if (State.bFunction) return MakeRejected("Functions cannot own root parameters.");
			ParameterId = Parameter->Metadata.Id = FGuid::NewGuid();
			const bool bTexture = Cast<DMaterialExpressionTextureParameter>(Expression.Get()) != nullptr;
			const auto BaseName = bTexture ? std::string("TextureParameter") : std::string(GetProgramTypeName(Entry.ResultType)) + "Parameter";
			Parameter->Metadata.Name = FName(BaseName);
			const auto* Material = Cast<DMaterial>(Owner.Get());
			for (uint32 Suffix = 1; Material->FindParameterDefinition(Parameter->Metadata.Name); ++Suffix)
				Parameter->Metadata.Name = FName(std::format("{}{}", BaseName, Suffix));
			DisplayName = Parameter->Metadata.DisplayName = Parameter->Metadata.Name.ToString();
		}
		if (Entry.AcceptedInputTypes.size() != Expression->GetAuthoredInputCount())
			return MakeRejected("The catalog input shape is stale.");
		bool bValidInputs = true;
		VisitMaterialExpressionInputs(*Expression, [&](uint32 Index, FMaterialExpressionInput& Input) {
			if (Index == 0 && FirstInput.ExpressionId.IsValid())
			{
				Input = FirstInput;
				if (!IsMaterialAdaptiveNumeric(Entry.Opcode)) return;
			}
			if (const auto* Sample = Cast<DMaterialExpressionTextureSample2D>(Expression.Get()); Sample && &Input == &Sample->UV) return;
			if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()); Sample && &Input == &Sample->UV) return;
			std::vector<float>* Default = nullptr;
			Expression->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (Property->GetValuePtr(Expression.Get()) != &Input) return;
				auto* Value = Expression->GetClass()->FindPropertyByName(FName(Property->NamePrivate.ToString() + "Default"));
				if (Value && Value->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
					&& static_cast<FArrayProperty*>(Value)->GetInner()->GetKind() == DurinCodeGen::EPropertyGenFlags::Float)
					Default = static_cast<std::vector<float>*>(Value->GetValuePtr(Expression.Get()));
			});
			const auto& Types = Entry.AcceptedInputTypes[Index];
			const auto Type = std::ranges::find_if(Types, [](auto T) { return T < EMaterialProgramValueType::Texture2D; });
			if (!Default || Type == Types.end()) { bValidInputs = false; return; }
			const auto Width = static_cast<uint32>(Cast<DMaterialExpressionSwizzle>(Expression.Get()) ? Entry.ResultType : *Type) + 1;
			float Value = 0;
			if (((Entry.Opcode == EMaterialProgramOpcode::Multiply || Entry.Opcode == EMaterialProgramOpcode::Divide) && Index == 1)
				|| (Entry.Opcode == EMaterialProgramOpcode::Clamp && Index == 2) || Entry.Opcode == EMaterialProgramOpcode::Normalize) Value = 1;
			else if (Entry.Opcode == EMaterialProgramOpcode::Lerp) Value = Index == 1 ? 1.f : Index == 2 ? .5f : 0.f;
			Default->assign(Width, Value);
			if (Cast<DMaterialExpressionMakeSurface>(Expression.Get()))
			{
				if (Index == 0) *Default = {.5f, .5f, .5f};
				if (Index == 1) *Default = {0, 0, 1};
				if (Index == 3) *Default = {.5f};
				if (Index == 4 || Index == 6 || Index == 7) *Default = {1};
			}
		});
		if (!bValidInputs) return MakeRejected("Create this node from a compatible texture or Surface output.");
		IncludeCallOutput(State, FirstInput);
		const auto Id = Expression->Id;
		State.Presentation.Nodes.push_back({Id, X, Y, DisplayName});
		State.Expressions.emplace_back(Expression.Get());
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Create Graph Expression", Transactions);
		if (Result)
		{
			Result.GeneratedNodeIds = Result.AffectedNodeIds = {Id};
			if (ParameterId.IsValid()) Result.AffectedParameterIds = {ParameterId};
		}
		return Result;
	}

	auto FMaterialGraphDocument::CreateExpression(const DMaterialExpression& Expression, int32 X, int32 Y,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		if (std::ranges::any_of(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; }))
			return MakeRejected("The expression GUID already exists.");
		auto* Copy = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Copy) return MakeRejected("Unable to copy the new expression.");
		State.Expressions.emplace_back(Copy);
		if (!Copy->Id.IsValid()) Copy->Id = FGuid::NewGuid();
		const auto Id = Copy->Id;
		FGuid ParameterId;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
		{
			if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error.empty()) return MakeRejected(Error);
			ParameterId = Parameter->Metadata.Id;
		}
		VisitMaterialExpressionInputs(*Copy, [&](uint32, FMaterialExpressionInput& Input) { IncludeCallOutput(State, Input); });
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Create Graph Expression", Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded)
		{
			Result.GeneratedNodeIds = {Id}; Result.AffectedNodeIds = {Id};
			if (ParameterId.IsValid()) Result.AffectedParameterIds.push_back(ParameterId);
		}
		return Result;
	}

	auto FMaterialGraphDocument::ReplaceExpression(const DMaterialExpression& Expression,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		const auto Existing = std::ranges::find_if(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; });
		if (Existing == State.Expressions.end()) return MakeRejected("The expression no longer exists.");
		auto* Copy = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Copy) return MakeRejected("Unable to copy the replacement expression.");
		TStrongObjectPtr<DMaterialExpression> Replacement(Copy);
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
			if (const auto Error = ResolveParameterExpression(State, *Parameter,
				Cast<DMaterialExpressionParameter>(Existing->Get())); !Error.empty()) return MakeRejected(Error);
		if ((*Existing)->GetClass() == Copy->GetClass())
		{
			if (!State.Assign(**Existing, *Copy)) return MakeRejected("Unable to update the expression properties.");
		}
		else *Existing = Replacement.Get();
		VisitMaterialExpressionInputs(*Copy, [&](uint32, FMaterialExpressionInput& Input) { IncludeCallOutput(State, Input); });
		const auto Id = Expression.Id;
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Replace Graph Expression", Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds.push_back(Id);
		return Result;
	}

	auto FMaterialGraphDocument::SetConstantValue(const FGuid& NodeId, const FMaterialParameterValue& Value,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Material && !Function) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions : Function->GetExpressionCollection().Expressions;
		const auto Existing = std::ranges::find_if(Expressions, [&](const auto& Expression) { return Expression->Id == NodeId; });
		if (Existing == Expressions.end() || !(Cast<DMaterialExpressionScalarConstant>(Existing->Get())
			|| Cast<DMaterialExpressionVector2Constant>(Existing->Get()) || Cast<DMaterialExpressionVector3Constant>(Existing->Get())
			|| Cast<DMaterialExpressionVector4Constant>(Existing->Get()))) return MakeRejected("The selected expression is not a constant.");
		const auto Literal = ReadParameterLiteral(GetProgramType(Value.GetType()), Value);
		if (!std::isfinite(Literal.X) || !std::isfinite(Literal.Y) || !std::isfinite(Literal.Z) || !std::isfinite(Literal.W))
			return MakeRejected("Constant values must be finite.", {{.Message = "Constant values must be finite."}});
		const auto EditValue = [&](auto* Constant, const auto& NewValue) {
			FGraphEditSession State(*Owner.Get());
			State.Modify(*Constant);
			Constant->Value = NewValue;
			return State.Commit("Edit Constant Expression", Transactions);
		};
		if (Value.GetType() == EMaterialParameterType::Scalar)
			if (auto* E = Cast<DMaterialExpressionScalarConstant>(Existing->Get())) return EditValue(E, Value.GetScalar());
		if (Value.GetType() == EMaterialParameterType::Vector2)
			if (auto* E = Cast<DMaterialExpressionVector2Constant>(Existing->Get())) return EditValue(E, Value.GetVector2());
		if (Value.GetType() == EMaterialParameterType::Vector)
			if (auto* E = Cast<DMaterialExpressionVector3Constant>(Existing->Get())) return EditValue(E, Value.GetVector());
		if (Value.GetType() == EMaterialParameterType::Vector4)
			if (auto* E = Cast<DMaterialExpressionVector4Constant>(Existing->Get())) return EditValue(E, Value.GetVector4());
		TStrongObjectPtr<DMaterialExpression> Replacement;
		switch (Value.GetType())
		{
		case EMaterialParameterType::Scalar:
			{ auto* Constant = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); Constant->Value = Value.GetScalar(); Replacement = TStrongObjectPtr<DMaterialExpression>(Constant); break; }
		case EMaterialParameterType::Vector2:
			{ auto* Constant = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); Constant->Value = Value.GetVector2(); Replacement = TStrongObjectPtr<DMaterialExpression>(Constant); break; }
		case EMaterialParameterType::Vector:
			{ auto* Constant = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None); Constant->Value = Value.GetVector(); Replacement = TStrongObjectPtr<DMaterialExpression>(Constant); break; }
		case EMaterialParameterType::Vector4:
			{ auto* Constant = NewObject<DMaterialExpressionVector4Constant>(nullptr, NAME_None); Constant->Value = Value.GetVector4(); Replacement = TStrongObjectPtr<DMaterialExpression>(Constant); break; }
		default: return MakeRejected("Constants require a numeric value.");
		}
		Replacement->Id = NodeId;
		return ReplaceExpression(*Replacement.Get(), Transactions);
	}

	auto FMaterialGraphDocument::SetSwizzleComponents(const FGuid& NodeId, std::span<const uint8> Components,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (Components.empty() || Components.size() > 4 || std::ranges::any_of(Components, [](uint8 Value) { return Value > 3; }))
			return MakeRejected("Swizzles require one to four valid components.");
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		for (auto& Expression : State.Expressions)
			if (Expression->Id == NodeId)
			{
				auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get());
				if (!Swizzle) return MakeRejected("The selected expression is not a swizzle.");
				State.Modify(*Swizzle);
				Swizzle->Components.assign(Components.begin(), Components.end());
				return CommitGraphEdit(*Owner.Get(), State, "Edit Swizzle", Transactions);
			}
		return MakeRejected("The expression no longer exists.");
	}

	auto FMaterialGraphDocument::RemoveNodes(std::span<const FGuid> NodeIds,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (NodeIds.size() > MaterialProgramMaxNodeCount) return MakeRejected("The removal selection exceeds the graph bound.");
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		const std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		if (std::ranges::any_of(State.Expressions, [&](const auto& E) { return Removed.contains(E->Id) && Cast<DMaterialExpressionMaterialOutput>(E.Get()); }))
			return MakeRejected("The material output node cannot be removed.");
		if (std::ranges::none_of(State.Expressions, [&](const auto& Expression) { return Removed.contains(Expression->Id); }))
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		std::erase_if(State.Expressions, [&](const auto& Expression) { return Removed.contains(Expression->Id); });
		std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Removed.contains(Position.NodeId); });
		for (auto& Expression : State.Expressions)
		{
			if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(Expression.Get()); Surface
				&& std::ranges::any_of(Surface->Attributes, [&](const auto& A) { return Removed.contains(A.Source.ExpressionId); }))
			{ State.Modify(*Surface); std::erase_if(Surface->Attributes, [&](const auto& A) { return Removed.contains(A.Source.ExpressionId); }); }
			VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) {
				if (Removed.contains(Input.ExpressionId)) { State.Modify(*Expression); Input = {}; }
			});
		}
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Remove Graph Nodes", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds.assign(Removed.begin(), Removed.end());
			std::ranges::sort(Result.AffectedNodeIds);
		}
		return Result;
	}

	auto FMaterialGraphDocument::ConnectInput(const FGuid& NodeId, uint32 InputIndex,
		FMaterialProgramLink Source, bool bReplaceExisting, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		const auto Node = std::ranges::find(State.Expressions, NodeId, [](const auto& Expression) { return Expression->Id; });
		if (Node == State.Expressions.end() || Cast<DMaterialExpressionFunctionCall>(Node->Get()))
			return MakeRejected("The graph input no longer exists.");
		FMaterialExpressionInput* Target = nullptr;
		VisitMaterialExpressionInputs(**Node, [&](uint32 Index, FMaterialExpressionInput& Input) {
			if (Index == InputIndex) Target = &Input;
		});
		if (!Target) return MakeRejected("The graph input no longer exists.");
		const FMaterialExpressionInput Connection{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
		if (!IsSourceAvailable(State, Connection)) return MakeRejected("The source output no longer exists.");
		if (Target->ExpressionId.IsValid() && *Target != Connection && !bReplaceExisting)
			return MakeRejected("The graph input is already connected.");
		if (*Target == Connection) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Cast<DMaterialExpressionMaterialOutput>(Node->Get()))
			return AssignMaterialOutput(InputIndex == static_cast<uint32>(EMaterialOutputPin::Surface)
				? std::nullopt : std::optional(static_cast<EMaterialSurfaceOutput>(InputIndex)), Source, Transactions);
		State.Modify(**Node);
		*Target = Connection;
		IncludeCallOutput(State, Connection);
		return CommitGraphEdit(*Owner.Get(), State, "Connect Graph Input", Transactions);
	}

	auto FMaterialGraphDocument::SetUseMaterialAttributes(bool bEnabled, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return MakeRejected("Only materials have a material output mode.");
		if (Material->GetExpressionOutputs().bUseMaterialAttributes == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FGraphEditSession State(*Material);
		State.GetOutputs().bUseMaterialAttributes = bEnabled;
		return CommitGraphEdit(*Material, State, "Change Material Output Mode", Transactions);
	}

	auto FMaterialGraphDocument::AssignMaterialOutput(std::optional<EMaterialSurfaceOutput> Attribute,
		FMaterialProgramLink Source, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return MakeRejected("Function documents expose output ports instead of Surface.");
		FGraphEditSession State(*Material);
		const FMaterialExpressionInput Connection{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
		if (!IsSourceAvailable(State, Connection)) return MakeRejected("The source output no longer exists.");
		const auto Previous = State.GetOutputs();
		const std::array Attributes{&State.GetOutputs().BaseColor, &State.GetOutputs().Normal, &State.GetOutputs().Metallic,
			&State.GetOutputs().Roughness, &State.GetOutputs().AmbientOcclusion, &State.GetOutputs().Emissive,
			&State.GetOutputs().Opacity, &State.GetOutputs().OpacityMask};
		if (Attribute)
		{
			const auto Index = static_cast<uint32>(*Attribute);
			if (Index >= Attributes.size()) return MakeRejected("The material output attribute is invalid.");
			*Attributes[Index] = Connection;
		}
		else
		{
			State.GetOutputs().Surface = Connection;
		}
		if (State.GetOutputs() == Previous) return {.Status = EMaterialGraphCommandStatus::NoChange};
		IncludeCallOutput(State, Connection);
		return CommitGraphEdit(*Material, State, "Connect Surface", Transactions);
	}

	auto FMaterialGraphDocument::InsertFunctionCall(DMaterialFunctionInterface& Function,
		int32 X, int32 Y, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		return InsertFunctionCall(Function, X, Y, {}, Transactions);
	}

	auto FMaterialGraphDocument::InsertFunctionCall(DMaterialFunctionInterface& Function,
		int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
		if (!Validation) return MakeRejected("The selected function dependency is invalid.", std::move(Validation.Diagnostics));
		if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
			return MakeRejected("This call would introduce recursive function dependencies.");
		const FGuid Id = FGuid::NewGuid();
		TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
		Call->Id = Id; Call->Function = &Function;
		for (const auto& Output : Function.GetFunctionSignature().Outputs) Call->Outputs.push_back({Output.Id, Output.Type});
		for (const auto& Input : Inputs)
		{
			FMaterialExpressionFunctionInputBinding Binding{Input.InputId, Input.ExpectedType,
				{Input.Source.SourceNodeId, Input.Source.SourceOutputIndex, Input.Source.SourceOutputId}};
			if (Input.Default.Kind != EMaterialInputDefaultKind::None)
			{
				const auto Count = static_cast<size_t>(Input.Default.Type) + 1;
				const std::array Values{Input.Default.Literal.X, Input.Default.Literal.Y, Input.Default.Literal.Z, Input.Default.Literal.W};
				if (Count > Values.size()) return MakeRejected("The function input default has an invalid numeric width.");
				Binding.InputDefault.assign(Values.begin(), Values.begin() + Count);
			}
			Call->Inputs.push_back(std::move(Binding));
		}
		Validation = ValidateMaterialFunctionCallSignature(*Call.Get(), Function.GetFunctionSignature(), EMaterialFunctionValidationMode::Editing);
		if (!Validation) return MakeRejected("The function call bindings are invalid.", std::move(Validation.Diagnostics));
		State.Expressions.emplace_back(Call.Get());
		for (const auto& Input : Call->Inputs) IncludeCallOutput(State, Input.Input);
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = CommitGraphEdit(*Owner.Get(), State, "Insert Function Call", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::ConnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
		FMaterialProgramLink Source, bool bReplaceExisting, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		auto* Call = FindCall(State, CallNodeId);
		if (!Call || !IsValid(Call->Function.Get())) return MakeRejected("The function call is unavailable.");
		const auto& Inputs = Call->Function->GetFunctionSignature().Inputs;
		const auto Port = std::ranges::find(Inputs, InputId, &FMaterialFunctionPort::Id);
		if (Port == Inputs.end()) return MakeRejected("The function input port no longer exists.");
		const FMaterialExpressionInput Connection{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
		if (!IsSourceAvailable(State, Connection)) return MakeRejected("The source output no longer exists.");
		auto Binding = std::ranges::find(Call->Inputs, InputId, &FMaterialExpressionFunctionInputBinding::InputId);
		if (Binding != Call->Inputs.end())
		{
			if (!bReplaceExisting && Binding->Input.ExpressionId.IsValid() && Binding->Input != Connection)
				return MakeRejected("The function input is already connected.");
			if (Binding->Input == Connection) return {.Status = EMaterialGraphCommandStatus::NoChange};
			State.Modify(*Call);
			Binding->Input = Connection;
		}
		else { State.Modify(*Call); Call->Inputs.push_back({InputId, Port->Type, Connection}); }
		IncludeCallOutput(State, Connection);
		return CommitGraphEdit(*Owner.Get(), State, "Connect Function Input", Transactions);
	}

	auto FMaterialGraphDocument::DisconnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FGraphEditSession State(*Owner.Get());
		auto* Call = FindCall(State, CallNodeId);
		if (!Call) return MakeRejected("The function call no longer exists.");
		const auto Binding = std::ranges::find(Call->Inputs, InputId, &FMaterialExpressionFunctionInputBinding::InputId);
		if (Binding == Call->Inputs.end()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Binding->Input == FMaterialExpressionInput{} && !Binding->InputDefault.empty())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		State.Modify(*Call);
		Binding->Input = {};
		if (Binding->InputDefault.empty()) Call->Inputs.erase(Binding);
		return CommitGraphEdit(*Owner.Get(), State, "Disconnect Function Input", Transactions);
	}
}
