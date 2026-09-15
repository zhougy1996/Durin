#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionBuild.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphExpressionState.h"
#include "DObject/Package.h"
#include "MaterialGraphValueTypes.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;
	namespace GraphEditInternals
	{
		// Resolve upstream widths before publishing the candidate. Owner validation still
		// checks selectors, cycles, fixed consumers and nonnumeric inputs atomically.
		auto AdaptNumericTypes(FOwnedGraphSnapshot& State) -> bool
		{
			using Type = EMaterialProgramValueType;
			const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
			std::map<FGuid, DMaterialExpression*> Expressions;
			std::map<FGuid, Type> Resolved;
			std::set<FGuid> Active;
			for (const auto& E : State.Expressions) Expressions.emplace(E->Id, E.Get());
			bool bValid = true;
			std::function<Type(const FMaterialExpressionInput&)> Resolve;
			Resolve = [&](const FMaterialExpressionInput& Input) -> Type {
				const auto Found = Expressions.find(Input.ExpressionId);
				if (Found == Expressions.end()) { bValid = false; return Type::Float; }
				auto* E = Found->second;
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(E))
				{
					const auto Port = std::ranges::find(Call->Outputs, Input.OutputId, &FMaterialFunctionOutputBinding::OutputId);
					if (Port != Call->Outputs.end()) return Port->ExpectedType;
					bValid = false; return Type::Float;
				}
				if (const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(E))
				{
					const auto Port = std::ranges::find(State.Signature.Inputs, Terminal->PortId, &FMaterialFunctionPort::Id);
					if (Port != State.Signature.Inputs.end()) return Port->Type;
					bValid = false; return Type::Float;
				}
				const auto Shape = std::ranges::find(Catalog, E->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Shape == Catalog.end()) { bValid = false; return Type::Float; }
				const auto Opcode = Shape->Opcode;
				if (Opcode == EMaterialProgramOpcode::TextureSampleParameter2D && Input.OutputIndex == 7) return Type::Texture2D;
				if (IsMaterialSamplingNode(Opcode) && Input.OutputIndex != 0)
					return Input.OutputIndex == 1 || Input.OutputIndex == 8 ? Type::Float3 : Type::Float;
				if (Opcode == EMaterialProgramOpcode::GetSurfaceAttributes && Input.OutputIndex < 8)
					return GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Input.OutputIndex));
				if (Resolved.contains(E->Id)) return Resolved.at(E->Id);
				if (!Active.insert(E->Id).second) { bValid = false; return Type::Float; }
				auto* Property = E->GetClass()->FindPropertyByName("ResultType");
				auto Result = Property ? *static_cast<Type*>(Property->GetValuePtr(E)) : Shape->ResultType;
				if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(E))
					Result = static_cast<Type>(Swizzle->Components.size() - 1);
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
					else Append->ResultType = Result = static_cast<Type>(AWidth + BWidth - 1);
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
					if (Property) *static_cast<Type*>(Property->GetValuePtr(E)) = Result;
					VisitMaterialExpressionInputs(*E, [&](uint32 Slot, FMaterialExpressionInput& Operand) {
						if (Opcode == EMaterialProgramOpcode::Lerp && Slot == 2) return;
						E->GetClass()->ForEachProperty([&](FProperty* P) {
							if (P->GetValuePtr(E) != &Operand) return;
							auto* Default = E->GetClass()->FindPropertyByName(FName(P->NamePrivate.ToString() + "Default"));
							if (!Default) return;
							auto& Values = *static_cast<std::vector<float>*>(Default->GetValuePtr(E));
							if (Values.size() > 1 && Values.size() != static_cast<size_t>(Result) + 1
								&& std::ranges::all_of(Values, [&](float V) { return V == Values.front(); })) Values.resize(1);
						});
					});
				}
				Active.erase(E->Id);
				Resolved.emplace(E->Id, Result);
				return Result;
			};
			for (const auto& E : State.Expressions)
			{
				const auto Shape = std::ranges::find(Catalog, E->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Shape != Catalog.end() && (IsMaterialAdaptiveNumeric(Shape->Opcode) || Shape->Opcode == EMaterialProgramOpcode::AppendVector)) Resolve({E->Id});
			}
			return bValid;
		}

		class FGraphDocumentChange final : public ITransactionCustomChange
		{
		public:
			FGraphDocumentChange(DObject& InOwner, FOwnedGraphSnapshot InBefore,
				FOwnedGraphSnapshot InAfter, std::string InDescription)
				: Owner(&InOwner), Before(std::move(InBefore)), After(std::move(InAfter)),
				Description(std::move(InDescription)), Packages{InOwner.GetPackage()} {}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto Undo() -> bool override { return Owner.IsValid() && Before.Apply(*Owner.Get()); }
			auto Redo() -> bool override { return Owner.IsValid() && After.Apply(*Owner.Get()); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				Before.AddReferencedObjects(Collector);
				After.AddReferencedObjects(Collector);
			}
			auto GetAllocatedSize() const -> size_t override
			{
				return Description.capacity() + Before.GetAllocatedSize() + After.GetAllocatedSize();
			}
		private:
			TWeakObjectPtr<DObject> Owner;
			mutable FOwnedGraphSnapshot Before, After;
			std::string Description;
			std::array<DPackage*, 1> Packages;
		};
		auto FindCall(FOwnedGraphSnapshot& State, const FGuid& Id) -> DMaterialExpressionFunctionCall*
		{
			for (auto& Expression : State.Expressions)
				if (Expression->Id == Id) return Cast<DMaterialExpressionFunctionCall>(Expression.Get());
			return nullptr;
		}
		auto IncludeCallOutput(FOwnedGraphSnapshot& State, const FMaterialExpressionInput& Source) -> void
		{
			if (!Source.OutputId.IsValid()) return;
			auto* Call = FindCall(State, Source.ExpressionId);
			if (!Call || !Call->Function.IsValid()
				|| std::ranges::find(Call->Outputs, Source.OutputId, &FMaterialFunctionOutputBinding::OutputId) != Call->Outputs.end()) return;
			const auto& Ports = Call->Function->GetFunctionSignature().Outputs;
			if (const auto Port = std::ranges::find(Ports, Source.OutputId, &FMaterialFunctionPort::Id); Port != Ports.end())
				Call->Outputs.push_back({Port->Id, Port->Type});
		}
		auto CommitOwnedExpressions(DObject& Owner, FOwnedGraphSnapshot Candidate,
			std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult
		{
			if (Transactions && Transactions->HasPendingOperation()) return MakeRejected("The editor transactor is busy.");
			auto* Function = Cast<DMaterialFunction>(&Owner);
			auto* Material = Cast<DMaterial>(&Owner);
			if (!Function && !Material) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
			if (Candidate.bFunction != (Function != nullptr)) return MakeRejected("The graph document kind cannot change.");
			if ((Function && Candidate.Outputs != FMaterialExpressionSurfaceOutputs{})
				|| (Material && Candidate.Signature != FMaterialFunctionSignature{})) return MakeRejected("The graph contains fields for a different document kind.");
			if (Candidate.Expressions.size() > MaterialProgramMaxNodeCount
				|| std::ranges::any_of(Candidate.Expressions, [](const auto& Expression) { return !Expression.Get(); }))
				return MakeRejected("The graph has too many expressions or contains a missing expression.");
			if (!AdaptNumericTypes(Candidate)) return MakeRejected("Numeric inputs require matching vector widths or scalar broadcasting; Append Vector requires a total of two to four components.");
			// Keep every terminal visible even when the source graph has no saved layout.
			std::erase_if(Candidate.Presentation.Nodes, [](const auto& Position) {
				return Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
					|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit;
			});
			for (size_t Index = 0; Function && Index < Candidate.Expressions.size(); ++Index)
			{
				const auto Id = Candidate.Expressions[Index]->Id;
				if (std::ranges::find(Candidate.Presentation.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)
					== Candidate.Presentation.Nodes.end())
					Candidate.Presentation.Nodes.push_back({Id, static_cast<int32>(Index % 4) * 320, static_cast<int32>(Index / 4) * 240});
			}
			FOwnedGraphSnapshot Before;
			if (Transactions && !Before.Capture(Owner)) return MakeRejected("Unable to snapshot the function before editing.");
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : Candidate.Expressions) Expressions.push_back(Expression.Get());
			if (!Candidate.MatchesGraph(Owner))
			{
				if (Function)
				{
					std::vector<DMaterialFunctionInterface*> Roots;
					for (const auto& Expression : Candidate.Expressions)
						if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Roots.push_back(Call->Function.Get());
					std::vector<FMaterialFunctionOwnerStamp> Closure;
					auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure);
					if (!Validation) return MakeRejected("The function dependencies are invalid.", std::move(Validation.Diagnostics));
					if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.GetObjectPath(); }))
						return MakeRejected("This change would introduce recursive function dependencies.");
				}
				const auto Validation = Function ? Function->SetFunctionExpressions(Candidate.Signature, Expressions)
					: Material->SetMaterialExpressions(Expressions, Candidate.Outputs);
				if (!Validation) return MakeRejected("The expression graph is invalid.", Validation.Diagnostics);
			}
			else if (Function ? Candidate.Presentation.Nodes == Function->GetFunctionPresentation().Nodes
				: Candidate.Presentation == Material->GetMaterialGraphPresentation())
				return {.Status = EMaterialGraphCommandStatus::NoChange};
			if (Function) Function->SetFunctionPresentation({.Nodes = Candidate.Presentation.Nodes});
			else Material->SetMaterialGraphPresentation(Candidate.Presentation);
			if (Transactions)
			{
				const auto bRecorded = Transactions->CommitApplied(std::make_unique<FGraphDocumentChange>(
					Owner, std::move(Before), std::move(Candidate), std::move(Description)));
				check(bRecorded);
			}
			return {.Status = EMaterialGraphCommandStatus::Succeeded};
		}

	}

	FMaterialGraphDocument::FMaterialGraphDocument(DObject& InOwner) : Owner(&InOwner) {}

	auto FMaterialGraphDocument::Capture(FMaterialGraphDocumentState& OutState) const -> bool
	{
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return false;
		OutState = std::move(static_cast<FMaterialGraphDocumentState&>(State));
		return true;
	}

	auto FMaterialGraphDocument::Commit(FMaterialGraphDocumentState Candidate,
		std::string Description, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FOwnedGraphSnapshot State;
		static_cast<FMaterialGraphDocumentState&>(State) = std::move(Candidate);
		// A caller can retain candidate handles. Transaction history must never alias them.
		for (auto& Expression : State.Expressions)
		{
			if (!Expression.Get()) return MakeRejected("The candidate contains a missing expression.");
			auto* Copy = DuplicateObject(Expression.Get(), nullptr, NAME_None);
			if (!Copy) return MakeRejected("Unable to copy the graph candidate.");
			Expression = TStrongObjectPtr<DMaterialExpression>(Copy);
		}
		for (const auto& Expression : State.Expressions)
			VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) { IncludeCallOutput(State, Input); });
		for (const auto* Output : {&State.Outputs.Surface, &State.Outputs.BaseColor, &State.Outputs.Normal, &State.Outputs.Metallic,
			&State.Outputs.Roughness, &State.Outputs.AmbientOcclusion, &State.Outputs.Emissive, &State.Outputs.Opacity, &State.Outputs.OpacityMask})
			IncludeCallOutput(State, *Output);
		return CommitOwnedExpressions(*Owner.Get(), std::move(State), std::move(Description), Transactions);
	}

	auto FMaterialGraphDocument::SetSignature(FMaterialFunctionSignature Signature,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return MakeRejected("Only function documents have an interface.");
		if (Signature == Function->GetFunctionSignature()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Function)) return MakeRejected("Unable to snapshot the function expressions.");
		State.Signature = std::move(Signature);
		// Terminal types are derived from the function signature, never written to nodes.
		return CommitOwnedExpressions(*Function, std::move(State), "Edit Function Interface", Transactions);
	}

	auto FMaterialGraphDocument::AddPort(bool bOutput, FMaterialFunctionPort Port,
		FMaterialProgramLink Source, int32 X, int32 Y, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return MakeRejected("Only function documents have an interface.");
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Function)) return MakeRejected("Unable to snapshot the function expressions.");
		if (!Port.Id.IsValid()) Port.Id = FGuid::NewGuid();
		const auto NodeId = FGuid::NewGuid();
		if (bOutput)
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->PortId = Port.Id;
			Terminal->Source = {Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
			State.Expressions.emplace_back(Terminal);
		}
		else
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->PortId = Port.Id;
			State.Expressions.emplace_back(Terminal);
		}
		(bOutput ? State.Signature.Outputs : State.Signature.Inputs).push_back(std::move(Port));
		State.Presentation.Nodes.push_back({NodeId, X, Y});
		auto Result = CommitOwnedExpressions(*Function, std::move(State),
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
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Function)) return MakeRejected("Unable to snapshot the function expressions.");
		auto& Ports = bOutput ? State.Signature.Outputs : State.Signature.Inputs;
		if (!std::erase_if(Ports, [&](const auto& Port) { return Port.Id == PortId; }))
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		std::erase_if(State.Expressions, [&](const auto& Expression) {
			if (bOutput)
			{
				const auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression.Get());
				return Terminal && Terminal->PortId == PortId;
			}
			const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(Expression.Get());
			return Terminal && Terminal->PortId == PortId;
		});
		return CommitOwnedExpressions(*Function, std::move(State), "Remove Function Port", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
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
		State.Expressions.push_back(std::move(Expression));
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Create Graph Expression", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
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
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Create Graph Expression", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Existing = std::ranges::find_if(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; });
		if (Existing == State.Expressions.end()) return MakeRejected("The expression no longer exists.");
		auto* Copy = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Copy) return MakeRejected("Unable to copy the replacement expression.");
		TStrongObjectPtr<DMaterialExpression> Replacement(Copy);
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
			if (const auto Error = ResolveParameterExpression(State, *Parameter,
				Cast<DMaterialExpressionParameter>(Existing->Get())); !Error.empty()) return MakeRejected(Error);
		*Existing = std::move(Replacement);
		VisitMaterialExpressionInputs(*Copy, [&](uint32, FMaterialExpressionInput& Input) { IncludeCallOutput(State, Input); });
		const auto Id = Expression.Id;
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Replace Graph Expression", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!Owner.IsValid() || !State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		for (auto& Expression : State.Expressions)
			if (Expression->Id == NodeId)
			{
				auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get());
				if (!Swizzle) return MakeRejected("The selected expression is not a swizzle.");
				Swizzle->Components.assign(Components.begin(), Components.end());
				return CommitOwnedExpressions(*Owner.Get(), std::move(State), "Edit Swizzle", Transactions);
			}
		return MakeRejected("The expression no longer exists.");
	}

	auto FMaterialGraphDocument::RemoveNodes(std::span<const FGuid> NodeIds,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (NodeIds.size() > MaterialProgramMaxNodeCount) return MakeRejected("The removal selection exceeds the graph bound.");
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		if (std::ranges::none_of(State.Expressions, [&](const auto& Expression) { return Removed.contains(Expression->Id); }))
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		for (const auto& Expression : State.Expressions)
		{
			if (!Removed.contains(Expression->Id)) continue;
			if (const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get()))
				std::erase_if(State.Signature.Inputs, [&](const auto& Port) { return Port.Id == Input->PortId; });
			if (const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get()))
				std::erase_if(State.Signature.Outputs, [&](const auto& Port) { return Port.Id == Output->PortId; });
		}
		std::erase_if(State.Expressions, [&](const auto& Expression) { return Removed.contains(Expression->Id); });
		std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Removed.contains(Position.NodeId); });
		for (auto& Expression : State.Expressions)
		{
			if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(Expression.Get()))
				std::erase_if(Surface->Attributes, [&](const auto& Attribute) { return Removed.contains(Attribute.Source.ExpressionId); });
			VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Input) {
				if (Removed.contains(Input.ExpressionId)) Input = {};
			});
		}
		for (auto* Output : {&State.Outputs.Surface, &State.Outputs.BaseColor, &State.Outputs.Normal, &State.Outputs.Metallic,
			&State.Outputs.Roughness, &State.Outputs.AmbientOcclusion, &State.Outputs.Emissive, &State.Outputs.Opacity, &State.Outputs.OpacityMask})
			if (Removed.contains(Output->ExpressionId)) *Output = {};
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Remove Graph Nodes", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Node = std::ranges::find(State.Expressions, NodeId, [](const auto& Expression) { return Expression->Id; });
		if (Node == State.Expressions.end() || Cast<DMaterialExpressionFunctionCall>(Node->Get()))
			return MakeRejected("The graph input no longer exists.");
		FMaterialExpressionInput* Target = nullptr;
		VisitMaterialExpressionInputs(**Node, [&](uint32 Index, FMaterialExpressionInput& Input) {
			if (Index == InputIndex) Target = &Input;
		});
		if (!Target) return MakeRejected("The graph input no longer exists.");
		const FMaterialExpressionInput Connection{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
		if (Target->ExpressionId.IsValid() && *Target != Connection && !bReplaceExisting)
			return MakeRejected("The graph input is already connected.");
		if (*Target == Connection) return {.Status = EMaterialGraphCommandStatus::NoChange};
		*Target = Connection;
		IncludeCallOutput(State, Connection);
		return CommitOwnedExpressions(*Owner.Get(), std::move(State), "Connect Graph Input", Transactions);
	}

	auto FMaterialGraphDocument::AssignMaterialOutput(std::optional<EMaterialSurfaceOutput> Attribute,
		FMaterialProgramLink Source, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return MakeRejected("Function documents expose output ports instead of Surface.");
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Material)) return MakeRejected("Unable to snapshot the material expressions.");
		const FMaterialExpressionInput Connection{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
		const auto Previous = State.Outputs;
		const std::array Attributes{&State.Outputs.BaseColor, &State.Outputs.Normal, &State.Outputs.Metallic,
			&State.Outputs.Roughness, &State.Outputs.AmbientOcclusion, &State.Outputs.Emissive,
			&State.Outputs.Opacity, &State.Outputs.OpacityMask};
		if (Attribute)
		{
			const auto Index = static_cast<uint32>(*Attribute);
			if (Index >= Attributes.size()) return MakeRejected("The material output attribute is invalid.");
			*Attributes[Index] = Connection;
			if (Connection.ExpressionId.IsValid()) State.Outputs.Surface = {};
		}
		else
		{
			State.Outputs.Surface = Connection;
			if (Connection.ExpressionId.IsValid()) for (auto* Output : Attributes) *Output = {};
		}
		if (State.Outputs == Previous) return {.Status = EMaterialGraphCommandStatus::NoChange};
		IncludeCallOutput(State, Connection);
		return CommitOwnedExpressions(*Material, std::move(State), "Connect Surface", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure);
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
		Validation = ValidateMaterialFunctionCallSignature(*Call.Get(), Function.GetFunctionSignature());
		if (!Validation) return MakeRejected("Bind the function's required inputs before inserting the call.", std::move(Validation.Diagnostics));
		State.Expressions.emplace_back(Call.Get());
		for (const auto& Input : Call->Inputs) IncludeCallOutput(State, Input.Input);
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = CommitOwnedExpressions(*Owner.Get(), std::move(State), "Insert Function Call", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::ConnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
		FMaterialProgramLink Source, bool bReplaceExisting, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Call = FindCall(State, CallNodeId);
		if (!Call || !IsValid(Call->Function.Get())) return MakeRejected("The function call is unavailable.");
		const auto& Inputs = Call->Function->GetFunctionSignature().Inputs;
		const auto Port = std::ranges::find(Inputs, InputId, &FMaterialFunctionPort::Id);
		if (Port == Inputs.end()) return MakeRejected("The function input port no longer exists.");
		const FMaterialExpressionInput Connection{Source.SourceNodeId, Source.SourceOutputIndex, Source.SourceOutputId};
		auto Binding = std::ranges::find(Call->Inputs, InputId, &FMaterialExpressionFunctionInputBinding::InputId);
		if (Binding != Call->Inputs.end())
		{
			if (!bReplaceExisting && Binding->Input.ExpressionId.IsValid() && Binding->Input != Connection)
				return MakeRejected("The function input is already connected.");
			if (Binding->Input == Connection) return {.Status = EMaterialGraphCommandStatus::NoChange};
			Binding->Input = Connection;
		}
		else Call->Inputs.push_back({InputId, Port->Type, Connection});
		IncludeCallOutput(State, Connection);
		return CommitOwnedExpressions(*Owner.Get(), std::move(State), "Connect Function Input", Transactions);
	}

	auto FMaterialGraphDocument::DisconnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (!Owner.IsValid()) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FOwnedGraphSnapshot State;
		if (!State.Capture(*Owner.Get())) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Call = FindCall(State, CallNodeId);
		if (!Call) return MakeRejected("The function call no longer exists.");
		const auto Binding = std::ranges::find(Call->Inputs, InputId, &FMaterialExpressionFunctionInputBinding::InputId);
		if (Binding == Call->Inputs.end()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Binding->Input == FMaterialExpressionInput{} && !Binding->InputDefault.empty())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		Binding->Input = {};
		if (Binding->InputDefault.empty()) Call->Inputs.erase(Binding);
		return CommitOwnedExpressions(*Owner.Get(), std::move(State), "Disconnect Function Input", Transactions);
	}
}
