#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionBuild.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphEditSession.h"
#include "DObject/Package.h"
#include "MaterialGraphValueTypes.h"
#include <cmath>
#include "Asset/Asset.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	namespace
	{
		auto SelectGraphSchema(const DObject& Owner) -> const FMaterialGraphSchema&
		{
			static const FMaterialSchema Material;
			static const FMaterialFunctionSchema Function;
			if (Cast<DMaterialFunction>(&Owner)) return Function;
			if (Cast<DMaterial>(&Owner)) return Material;
			check(false); std::terminate();
		}

		struct FCreationFunctionResult
		{
			DMaterialFunctionInterface* Function = nullptr;
			std::string Message;
			explicit operator bool() const { return Function != nullptr; }
		};
		auto LoadCreationFunction(const std::string& Name) -> FCreationFunctionResult
		{
			FTopLevelAssetPath Path;
			if (const auto Parsed = FTopLevelAssetPath::TryCreateWithDiagnostic(Name, Path); !Parsed)
				return {.Message = "The material function path is invalid. " + FormatObjectError(Parsed.Error)};
			DMaterialFunctionInterface* Function = nullptr;
			if (auto Loaded = LoadObject(Path, Function); !Loaded)
				return {.Message = "Unable to load the material function. " + Loaded.Message};
			return {.Function = Function};
		}

	}
	auto FMaterialGraphDocument::CanCreate(const FMaterialGraphCreationAction& Action,
		std::optional<EMaterialProgramValueType> SourceType) const -> bool
	{
		if (!Owner.IsValid()) return false;
		if (!Schema.CanCreate(Action, SourceType)) return false;
		if (!std::holds_alternative<std::string>(Action.Payload)) return true;
		if (!SourceType) return true;
		const auto Loaded = LoadCreationFunction(std::get<std::string>(Action.Payload));
		return Loaded && std::ranges::any_of(Loaded.Function->GetFunctionSignature().Inputs,
			[&](const auto& Port) { return FMaterialGraphSchema::AcceptsPort(Port.Type, *SourceType); });
	}
	auto FMaterialGraphDocument::Create(const FMaterialGraphCreationRequest& Request,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FMaterialProgramLink Source;
		std::optional<EMaterialProgramValueType> SourceType;
		if (Request.Source)
		{
			const auto& Address = *Request.Source;
			if ((Address.Kind != EMaterialGraphPinKind::Output && Address.Kind != EMaterialGraphPinKind::FunctionOutput)
				|| (Address.Kind == EMaterialGraphPinKind::FunctionOutput) != Address.PortId.IsValid())
				return RejectCommand("Creation requires an output pin.");
			const auto View = Inspect();
			for (const auto& Node : View.Nodes)
				if (Node.Node.Id == Address.NodeId)
					for (const auto& Pin : Node.Outputs)
						if (Pin.OutputIndex == Address.Index && Pin.PortId == Address.PortId && !Pin.bMissing)
							SourceType = Pin.Type;
			if (!SourceType) return RejectCommand("The source output no longer exists.");
			Source = {Address.NodeId, static_cast<uint8>(Address.Index), Address.PortId};
		}

		if (!Schema.CanCreate(Request.Action, SourceType))
			return RejectCommand("This action is not compatible with the graph or source output.");
		if (const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Request.Action.Payload))
			return CreateCatalogNode(*Entry, Request.X, Request.Y,
				{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId}, Transactions);
		if (const auto* Spec = std::get_if<FMaterialGraphPortCreation>(&Request.Action.Payload))
		{
			const auto* Function = Cast<DMaterialFunction>(Owner.Get());
			if (!Function) return RejectCommand("Only function graphs have ports.");
			const auto& Signature = Function->GetFunctionSignature();
			const auto& Ports = Spec->bOutput ? Signature.Outputs : Signature.Inputs;
			FMaterialFunctionPort Port;
			Port.Type = Spec->Type;
			for (uint32 Index = 1;; ++Index)
			{
				Port.Name = std::format("{} {}", Spec->bOutput ? "Output" : "Input", Index);
				if (std::ranges::none_of(Ports, [&](const auto& Existing) { return Existing.Name == Port.Name; })) break;
			}
			if (!Spec->bOutput)
				Port.Default.Kind = Port.Type == EMaterialProgramValueType::Surface ? EMaterialFunctionDefaultKind::Surface
					: Port.Type == EMaterialProgramValueType::Texture2D ? EMaterialFunctionDefaultKind::Texture
					: EMaterialFunctionDefaultKind::Numeric;
			return AddPort(Spec->bOutput, Port, Source, Request.X, Request.Y, Transactions);
		}
		auto Loaded = LoadCreationFunction(std::get<std::string>(Request.Action.Payload));
		if (!Loaded)
		{
			return RejectCommand(std::move(Loaded.Message));
		}
		auto* Function = Loaded.Function;
		std::vector<FMaterialFunctionInputBinding> Inputs;
		if (SourceType)
		{
			for (const auto& Port : Function->GetFunctionSignature().Inputs)
				if (FMaterialGraphSchema::AcceptsPort(Port.Type, *SourceType)) { Inputs.push_back({Port.Id, Port.Type, Source}); break; }
			if (Inputs.empty()) return RejectCommand("This function has no compatible input.");
		}
		return InsertFunctionCall(*Function, Request.X, Request.Y, Inputs, Transactions);
	}
	namespace GraphEditInternals
	{
		// Update inferred widths on live objects, recording each participant before
		// writing. Unresolvable types remain compiler diagnostics during editing.
		auto AdaptNumericTypes(FGraphEditSession& State, std::span<const FGuid> ChangedNodes, std::span<const FGuid> ChangedOutputNodes) -> bool
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
						else if (const auto* Default = FindMaterialExpressionInputDefault(*E, Operand))
						{
							const auto& Values = *Default;
							// Uniform defaults are width-independent; retain authored components otherwise.
							if (Values.size() > 1 && !std::ranges::all_of(Values, [&](float V) { return V == Values.front(); }))
								Merge(static_cast<Type>(Values.size() - 1));
						}
					});
					if (bOperand) Result = Inferred;
					if (!GetMaterialProgramNodeSignature(Opcode, Result)) bValid = false;
					if (Property && *static_cast<Type*>(Property->GetValuePtr(E)) != Result)
					{ State.Modify(*E); *static_cast<Type*>(Property->GetValuePtr(E)) = Result; }
					VisitMaterialExpressionInputs(*E, [&](uint32 Slot, FMaterialExpressionInput& Operand) {
						if (Opcode == EMaterialProgramOpcode::Lerp && Slot == 2) return;
						if (auto* Default = FindMaterialExpressionInputDefault(*E, Operand))
						{
							auto& Values = *Default;
							if (Values.size() > 1 && Values.size() != static_cast<size_t>(Result) + 1
								&& std::ranges::all_of(Values, [&](float V) { return V == Values.front(); })) { State.Modify(*E); Values.resize(1); }
						}
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

		auto IsSourceAvailable(const FGraphEditSession& State, const FMaterialExpressionInput& Input) -> bool
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

		auto FindCall(FGraphEditSession& State, const FGuid& Id) -> DMaterialExpressionFunctionCall*
		{
			for (auto& Expression : State.Expressions)
				if (Expression->Id == Id) return Cast<DMaterialExpressionFunctionCall>(Expression.Get());
			return nullptr;
		}
		auto IncludeCallOutput(FGraphEditSession& State, const FMaterialExpressionInput& Source) -> void
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

	FMaterialGraphDocument::FMaterialGraphDocument(DObject& InOwner)
		: Owner(&InOwner), Schema(SelectGraphSchema(InOwner)) {}

	auto FMaterialGraphDocument::SetPort(bool bOutput, FMaterialFunctionPort Port,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return RejectCommand("Only function documents have an interface.");
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
			return State.Commit("Edit Function Port", Transactions);
		}
		return RejectCommand("The function port is unavailable.");
	}

	auto FMaterialGraphDocument::AddPort(bool bOutput, FMaterialFunctionPort Port,
		FMaterialProgramLink Source, int32 X, int32 Y, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return RejectCommand("Only function documents have an interface.");
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
		auto Result = State.Commit(
			bOutput ? "Add Function Output" : "Add Function Input", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {NodeId};
		return Result;
	}

	auto FMaterialGraphDocument::RemovePort(bool bOutput, const FGuid& PortId,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return RejectCommand("Only function documents have an interface.");
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
		return State.Commit("Remove Function Port", Transactions);
	}

	auto FMaterialGraphDocument::CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X, int32 Y,
		FMaterialExpressionInput FirstInput, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
		if (std::ranges::none_of(Catalog, [&](const auto& Expected) {
			return Entry.Opcode == Expected.Opcode && Entry.ResultType == Expected.ResultType
				&& Entry.ExpressionClass == Expected.ExpressionClass && Entry.AcceptedInputTypes == Expected.AcceptedInputTypes;
		})) return RejectCommand("The catalog expression shape is stale.");
		if (FirstInput.ExpressionId.IsValid() && Entry.AcceptedInputTypes.empty())
			return RejectCommand("This catalog expression has no input pin.");
		FGraphEditSession State(*Owner.Get());
		TStrongObjectPtr<DMaterialExpression> Expression(NewObject<DMaterialExpression>(Entry.ExpressionClass, nullptr, NAME_None));
		if (!Expression) return RejectCommand("The catalog expression class is unavailable.");
		Expression->Id = FGuid::NewGuid();
		if (auto* Property = Expression->GetClass()->FindPropertyByName("ResultType"))
			*static_cast<EMaterialProgramValueType*>(Property->GetValuePtr(Expression.Get())) = Entry.ResultType;
		if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()))
		{
			if (Entry.ResultType > EMaterialProgramValueType::Float4) return RejectCommand("The swizzle width is invalid.");
			Swizzle->Components.clear();
			for (uint8 Index = 0; Index <= static_cast<uint8>(Entry.ResultType); ++Index) Swizzle->Components.push_back(Index);
		}
		FGuid ParameterId;
		std::string DisplayName;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
		{
			if (!Schema.CanOwnParameters()) return RejectCommand("Functions cannot own root parameters.");
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
			return RejectCommand("The catalog input shape is stale.");
		std::optional<uint32> InvalidInput;
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
			if (!Default || Type == Types.end()) { if (!InvalidInput) InvalidInput = Index; return; }
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
		if (InvalidInput) return RejectCommand("Create this node from a compatible texture or Surface output.");
		IncludeCallOutput(State, FirstInput);
		const auto Id = Expression->Id;
		State.Presentation.Nodes.push_back({Id, X, Y, DisplayName});
		State.Expressions.emplace_back(Expression.Get());
		auto Result = State.Commit("Create Graph Expression", Transactions);
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
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (!Schema.CanCreateExpression(Expression)) return RejectCommand("This expression is not supported by the graph.");
		FGraphEditSession State(*Owner.Get());
		if (std::ranges::any_of(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; }))
			return RejectCommand("The expression GUID already exists.");
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		auto* Copy = Duplicated.Object;
		if (!Copy) return RejectCommand(FormatObjectGraphError(Duplicated.Error));
		State.Expressions.emplace_back(Copy);
		if (!Copy->Id.IsValid()) Copy->Id = FGuid::NewGuid();
		const auto Id = Copy->Id;
		FGuid ParameterId;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
		{
			if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return Error;
			ParameterId = Parameter->Metadata.Id;
		}
		VisitMaterialExpressionInputs(*Copy, [&](uint32, FMaterialExpressionInput& Input) { IncludeCallOutput(State, Input); });
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = State.Commit("Create Graph Expression", Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded)
		{
			Result.GeneratedNodeIds = {Id}; Result.AffectedNodeIds = {Id};
			if (ParameterId.IsValid()) Result.AffectedParameterIds.push_back(ParameterId);
		}
		return Result;
	}

	auto FMaterialGraphDocument::ReplaceExpression(const DMaterialExpression& Expression,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FGraphEditSession State(*Owner.Get());
		const auto Existing = std::ranges::find_if(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; });
		if (Existing == State.Expressions.end()) return RejectCommand("The expression no longer exists.");
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		auto* Copy = Duplicated.Object;
		if (!Copy) return RejectCommand(FormatObjectGraphError(Duplicated.Error));
		TStrongObjectPtr<DMaterialExpression> Replacement(Copy);
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
			if (const auto Error = ResolveParameterExpression(State, *Parameter,
				Cast<DMaterialExpressionParameter>(Existing->Get())); !Error) return Error;
		if ((*Existing)->GetClass() == Copy->GetClass())
		{
			if (const auto Assigned = State.Assign(**Existing, *Copy); !Assigned) return Assigned;
		}
		else *Existing = Replacement.Get();
		VisitMaterialExpressionInputs(*Copy, [&](uint32, FMaterialExpressionInput& Input) { IncludeCallOutput(State, Input); });
		const auto Id = Expression.Id;
		auto Result = State.Commit("Replace Graph Expression", Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds.push_back(Id);
		return Result;
	}

	auto FMaterialGraphDocument::SetConstantValue(const FGuid& NodeId, const FMaterialParameterValue& Value,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Material && !Function) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions : Function->GetExpressionCollection().Expressions;
		const auto Existing = std::ranges::find_if(Expressions, [&](const auto& Expression) { return Expression->Id == NodeId; });
		if (Existing == Expressions.end() || !(Cast<DMaterialExpressionScalarConstant>(Existing->Get())
			|| Cast<DMaterialExpressionVector2Constant>(Existing->Get()) || Cast<DMaterialExpressionVector3Constant>(Existing->Get())
			|| Cast<DMaterialExpressionVector4Constant>(Existing->Get()))) return RejectCommand("The selected expression is not a constant.");
		const auto Literal = ReadParameterLiteral(GetProgramType(Value.GetType()), Value);
		if (!std::isfinite(Literal.X) || !std::isfinite(Literal.Y) || !std::isfinite(Literal.Z) || !std::isfinite(Literal.W))
			return RejectCommand("Constant values must be finite.", {{.NodeId = NodeId, .Error = EMaterialExpressionError::NonFiniteConstant}});
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
		default: return RejectCommand("Constants require a numeric value.");
		}
		Replacement->Id = NodeId;
		return ReplaceExpression(*Replacement.Get(), Transactions);
	}

	auto FMaterialGraphDocument::SetSwizzleComponents(const FGuid& NodeId, std::span<const uint8> Components,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		const auto Invalid = std::ranges::find_if(Components, [](uint8 Value) { return Value > 3; });
		if (Components.empty() || Components.size() > 4 || Invalid != Components.end())
		{
			return RejectCommand("Swizzles require one to four valid components.");
		}
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FGraphEditSession State(*Owner.Get());
		for (auto& Expression : State.Expressions)
			if (Expression->Id == NodeId)
			{
				auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get());
				if (!Swizzle) return RejectCommand("The selected expression is not a swizzle.");
				State.Modify(*Swizzle);
				Swizzle->Components.assign(Components.begin(), Components.end());
				return State.Commit("Edit Swizzle", Transactions);
			}
		return RejectCommand("The expression no longer exists.");
	}

	auto FMaterialGraphDocument::RemoveNodes(std::span<const FGuid> NodeIds,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (NodeIds.size() > MaterialProgramMaxNodeCount) return RejectCommand("The removal selection exceeds the graph bound.");
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FGraphEditSession State(*Owner.Get());
		const std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		const auto Output = std::ranges::find_if(State.Expressions, [&](const auto& E) { return Removed.contains(E->Id) && !Schema.CanRemove(*E.Get()); });
		if (Output != State.Expressions.end())
			return RejectCommand("The material output node cannot be removed.");
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
		auto Result = State.Commit("Remove Graph Nodes", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds.assign(Removed.begin(), Removed.end());
			std::ranges::sort(Result.AffectedNodeIds);
		}
		return Result;
	}

	auto FMaterialGraphDocument::Connect(const FMaterialGraphPinAddress& TargetAddress,
		const FMaterialGraphPinAddress& SourceAddress, bool bReplaceExisting, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (!Schema.IsSourceAddressValid(SourceAddress))
			return RejectCommand("The source must be an output pin.");
		if (!Schema.IsSourceKeyValid(SourceAddress))
			return RejectCommand("The source pin key is invalid.");
		FGraphEditSession State(*Owner.Get());
		const auto Node = std::ranges::find(State.Expressions, TargetAddress.NodeId, [](const auto& E) { return E->Id; });
		if (Node == State.Expressions.end()) return RejectCommand("The graph input no longer exists.");
		FMaterialExpressionInput* Target = nullptr;
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Node->Get());
		const FMaterialFunctionPort* Port = nullptr;
		bool bRemoveBinding = false;
		if (TargetAddress.Kind == EMaterialGraphPinKind::FunctionInput)
		{
			if (!Call || !TargetAddress.PortId.IsValid()) return RejectCommand("The function call is unavailable.");
			if (IsValid(Call->Function.Get()))
			{
				const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
				const auto Found = std::ranges::find(Ports, TargetAddress.PortId, &FMaterialFunctionPort::Id);
				if (Found != Ports.end()) Port = &*Found;
			}
			if (!Port && SourceAddress.NodeId.IsValid()) return RejectCommand("The function input port no longer exists.");
			const auto Binding = std::ranges::find(Call->Inputs, TargetAddress.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
			if (Binding != Call->Inputs.end())
			{
				Target = &Binding->Input;
				bRemoveBinding = !SourceAddress.NodeId.IsValid() && Binding->InputDefault.empty();
			}
		}
		else
		{
			if (Call || TargetAddress.PortId.IsValid()) return RejectCommand("The input pin key is invalid.");
			uint32 Index = TargetAddress.Index;
			switch (TargetAddress.Kind)
			{
			case EMaterialGraphPinKind::Input: break;
			case EMaterialGraphPinKind::MaterialAttribute:
				if (!Cast<DMaterialExpressionMaterialOutput>(Node->Get()) || Index >= 8)
					return RejectCommand("The material attribute pin is invalid.");
				break;
			case EMaterialGraphPinKind::MaterialSurface:
				if (!Cast<DMaterialExpressionMaterialOutput>(Node->Get())) return RejectCommand("The material output is unavailable.");
				Index = static_cast<uint32>(EMaterialOutputPin::Surface); break;
			default: return RejectCommand("The target must be an input pin.");
			}
			VisitMaterialExpressionInputs(**Node, [&](uint32 Slot, FMaterialExpressionInput& Input) { if (Slot == Index) Target = &Input; });
			if (!Target) return RejectCommand("The graph input no longer exists.");
		}
		const FMaterialExpressionInput Connection{SourceAddress.NodeId, static_cast<uint8>(SourceAddress.Index), SourceAddress.PortId};
		if (!IsSourceAvailable(State, Connection)) return RejectCommand("The source output no longer exists.");
		const auto Previous = Target ? *Target : FMaterialExpressionInput{};
		if (!Schema.CanReplaceConnection(Previous, Connection, bReplaceExisting))
			return RejectCommand("The graph input is already connected.");
		if (Previous == Connection && !bRemoveBinding) return {.Status = EMaterialGraphCommandStatus::NoChange};
		State.Modify(**Node);
		if (Target) *Target = Connection;
		else Call->Inputs.push_back({Port->Id, Port->Type, Connection});
		if (Call && !Connection.ExpressionId.IsValid())
			std::erase_if(Call->Inputs, [&](const auto& Binding) {
				return Binding.InputId == TargetAddress.PortId && Binding.InputDefault.empty();
			});
		IncludeCallOutput(State, Connection);
		auto Result = State.Commit("Connect Graph Input", Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded)
		{
			Result.AffectedNodeIds.push_back(TargetAddress.NodeId);
			if (Previous.ExpressionId.IsValid()) Result.AffectedNodeIds.push_back(Previous.ExpressionId);
			if (Connection.ExpressionId.IsValid()) Result.AffectedNodeIds.push_back(Connection.ExpressionId);
			std::ranges::sort(Result.AffectedNodeIds);
			const auto Duplicates = std::ranges::unique(Result.AffectedNodeIds);
			Result.AffectedNodeIds.erase(Duplicates.begin(), Duplicates.end());
		}
		return Result;
	}

	auto FMaterialGraphDocument::SetUseMaterialAttributes(bool bEnabled, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return RejectCommand("Only materials have a material output mode.");
		if (Material->GetExpressionOutputs().bUseMaterialAttributes == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FGraphEditSession State(*Material);
		State.GetOutputs().bUseMaterialAttributes = bEnabled;
		return State.Commit("Change Material Output Mode", Transactions);
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
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FGraphEditSession State(*Owner.Get());
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
		if (!Validation) return RejectCommand("The selected function dependency is invalid.", std::move(Validation.Diagnostics));
		if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
			return RejectCommand("This call would introduce recursive function dependencies.");
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
				if (Count > Values.size()) return RejectCommand("The function input default has an invalid numeric width.");
				Binding.InputDefault.assign(Values.begin(), Values.begin() + Count);
			}
			Call->Inputs.push_back(std::move(Binding));
		}
		Validation = ValidateMaterialFunctionCallSignature(*Call.Get(), Function.GetFunctionSignature(), EMaterialFunctionValidationMode::Editing);
		if (!Validation) return RejectCommand("The function call bindings are invalid.", std::move(Validation.Diagnostics));
		State.Expressions.emplace_back(Call.Get());
		for (const auto& Input : Call->Inputs) IncludeCallOutput(State, Input.Input);
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = State.Commit("Insert Function Call", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::Disconnect(const FMaterialGraphPinAddress& Target,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		return Connect(Target, FMaterialGraphPinAddress::Output({}), true, Transactions);
	}
}
