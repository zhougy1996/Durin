#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "DObject/Package.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;
	namespace
	{
		class FGraphDocumentChange final : public ITransactionCustomChange
		{
		public:
			FGraphDocumentChange(DObject& InOwner, FMaterialGraphDocumentState InBefore,
				FMaterialGraphDocumentState InAfter, std::string InDescription)
				: Owner(&InOwner), Before(std::move(InBefore)), After(std::move(InAfter)),
				Description(std::move(InDescription)), Packages{InOwner.GetPackage()} {}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto Undo() -> bool override { return Apply(Before); }
			auto Redo() -> bool override { return Apply(After); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (const auto* State : {&Before, &After})
				{
					for (const auto& Call : State->Calls)
						if (DObject* Function = Call.Function.Get()) Collector.AddReferencedObject(Function);
					for (const auto& Definition : State->Definitions)
						if (DObject* Texture = Definition.Value.TextureValue.Get()) Collector.AddReferencedObject(Texture);
				}
			}
			auto GetAllocatedSize() const -> size_t override
			{
				size_t Bytes = Description.capacity();
				for (const auto* State : {&Before, &After})
				{
					Bytes += State->Program.Nodes.capacity() * sizeof(FMaterialProgramNode)
						+ State->Definitions.capacity() * sizeof(FMaterialParameterDefinition)
						+ State->Calls.capacity() * sizeof(FMaterialFunctionCall)
						+ State->Presentation.Nodes.capacity() * sizeof(FMaterialGraphNodePresentation);
					for (const auto& Node : State->Program.Nodes)
						Bytes += Node.DisplayName.capacity() + Node.Inputs.capacity() * sizeof(FMaterialProgramLink)
							+ Node.SurfaceAttributes.capacity() * sizeof(FMaterialSurfaceAttributeBinding);
					for (const auto& Call : State->Calls)
						Bytes += Call.Inputs.capacity() * sizeof(FMaterialFunctionInputBinding)
							+ Call.Outputs.capacity() * sizeof(FMaterialFunctionOutputBinding);
					for (const auto* Ports : {&State->Signature.Inputs, &State->Signature.Outputs})
					{
						Bytes += Ports->capacity() * sizeof(FMaterialFunctionPort);
						for (const auto& Port : *Ports) Bytes += Port.Name.capacity();
					}
					for (const auto& Definition : State->Definitions) Bytes += Definition.DisplayName.capacity();
				}
				return Bytes;
			}
		private:
			auto Apply(const FMaterialGraphDocumentState& State) -> bool
			{
				return Owner.IsValid() && static_cast<bool>(FMaterialGraphDocument(*Owner.Get()).Commit(State, Description));
			}
			TWeakObjectPtr<DObject> Owner;
			FMaterialGraphDocumentState Before, After;
			std::string Description;
			std::array<DPackage*, 1> Packages;
		};
	}

	FMaterialGraphDocument::FMaterialGraphDocument(DObject& InOwner) : Owner(&InOwner) {}

	auto FMaterialGraphDocument::Capture(FMaterialGraphDocumentState& OutState) const -> bool
	{
		FMaterialGraphDocumentState State;
		if (const auto* Material = Cast<DMaterial>(Owner.Get()))
		{
			State.Program = *Material->GetMaterialProgram();
			State.Definitions.assign(Material->GetParameterDefinitions().begin(), Material->GetParameterDefinitions().end());
			State.Calls.assign(Material->GetMaterialFunctionCalls().begin(), Material->GetMaterialFunctionCalls().end());
			State.Presentation = Material->GetMaterialGraphPresentation();
		}
		else if (const auto* Function = Cast<DMaterialFunction>(Owner.Get()))
		{
			State.bFunction = true;
			State.Program.Nodes = Function->GetFunctionGraph().Nodes;
			State.Signature = Function->GetFunctionSignature();
			State.Calls = Function->GetFunctionGraph().Calls;
			State.Presentation.Nodes = Function->GetFunctionPresentation().Nodes;
		}
		else return false;
		OutState = std::move(State);
		return true;
	}

	auto FMaterialGraphDocument::Commit(FMaterialGraphDocumentState Candidate,
		std::string Description, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState Before;
		if (!Capture(Before)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (Transactions && Transactions->HasPendingOperation()) return MakeRejected("The editor transactor is busy.");
		if (Candidate.bFunction != Before.bFunction) return MakeRejected("The graph document kind cannot change.");
		if (Candidate.Program.SchemaVersion != CurrentMaterialProgramSchemaVersion)
			return MakeRejected("The graph document program schema is unsupported.");
		Candidate.Presentation = SanitizeMaterialGraphPresentation(Candidate.Presentation, Candidate.Program);
		if (Candidate.bFunction)
		{
			if (!Candidate.Definitions.empty() || Candidate.Program.Outputs != FMaterialSurfaceOutputs{})
				return MakeRejected("Function graphs cannot own root parameters or material output bindings.");
			Candidate.Presentation.bHasMaterialOutputPosition = false;
			Candidate.Presentation.MaterialOutputX = Candidate.Presentation.MaterialOutputY = 0;
			for (size_t Index = 0; Index < Candidate.Program.Nodes.size(); ++Index)
			{
				const auto Id = Candidate.Program.Nodes[Index].Id;
				if (std::ranges::find(Candidate.Presentation.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)
					== Candidate.Presentation.Nodes.end())
					Candidate.Presentation.Nodes.push_back({Id, static_cast<int32>(Index % 4) * 320, static_cast<int32>(Index / 4) * 240});
			}
			std::ranges::sort(Candidate.Presentation.Nodes, {}, &FMaterialGraphNodePresentation::NodeId);
		}
		else if (Candidate.Signature != FMaterialFunctionSignature{})
			return MakeRejected("Material graphs cannot declare function ports.");
		const auto IncludeOutput = [&](const FMaterialProgramLink& Source) {
			if (!Source.SourceOutputId.IsValid()) return;
			auto Call = std::ranges::find(Candidate.Calls, Source.SourceNodeId, &FMaterialFunctionCall::NodeId);
			if (Call == Candidate.Calls.end() || !Call->Function.IsValid()
				|| std::ranges::find(Call->Outputs, Source.SourceOutputId, &FMaterialFunctionOutputBinding::OutputId) != Call->Outputs.end()) return;
			const auto& Ports = Call->Function->GetFunctionSignature().Outputs;
			if (const auto Port = std::ranges::find(Ports, Source.SourceOutputId, &FMaterialFunctionPort::Id); Port != Ports.end())
				Call->Outputs.push_back({Port->Id, Port->Type});
		};
		for (const auto& Node : Candidate.Program.Nodes)
		{
			for (const auto& Input : Node.Inputs) IncludeOutput(Input);
			for (const auto& Attribute : Node.SurfaceAttributes) IncludeOutput(Attribute.Source);
		}
		for (const auto& Call : Candidate.Calls) for (const auto& Input : Call.Inputs) IncludeOutput(Input.Source);
		IncludeOutput(Candidate.Program.Outputs.Surface);
		for (uint32 Index = 0; Index < 8; ++Index) IncludeOutput(GetMaterialSurfaceOutputLink(Candidate.Program.Outputs, static_cast<EMaterialSurfaceOutput>(Index)));
		if (Before == Candidate) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Candidate.bFunction && Candidate.Calls != Before.Calls)
		{
			std::vector<DMaterialFunctionInterface*> Roots;
			for (const auto& Call : Candidate.Calls) Roots.push_back(Call.Function.Get());
			FMaterialFunctionClosure Closure;
			auto Validation = SnapshotMaterialFunctionClosure(Roots, Closure);
			if (!Validation) return MakeRejected("The function dependencies are invalid.", std::move(Validation.Diagnostics));
			if (std::ranges::any_of(Closure.Functions, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
				return MakeRejected("This change would introduce recursive function dependencies.");
		}
		if (auto* Function = Cast<DMaterialFunction>(Owner.Get()))
		{
			FMaterialFunctionGraph Graph;
			Graph.Signature = Candidate.Signature;
			Graph.Nodes = Candidate.Program.Nodes;
			Graph.Calls = Candidate.Calls;
			auto Validation = Function->SetFunctionGraph(std::move(Graph));
			if (!Validation) return MakeRejected("The function graph is invalid.", std::move(Validation.Diagnostics));
			Function->SetFunctionPresentation({.Nodes = Candidate.Presentation.Nodes});
		}
		else
		{
			auto* Material = Cast<DMaterial>(Owner.Get());
			if (Candidate.Definitions == Before.Definitions)
			{
				auto Validation = Material->SetMaterialProgramAndFunctionCalls(Candidate.Program, Candidate.Calls);
				if (!Validation) return MakeRejected("The material graph is invalid.", std::move(Validation.Diagnostics));
			}
			else
			{
				auto Validation = Material->SetMaterialDefinitionsAndProgram(Candidate.Definitions, Candidate.Program, Candidate.Calls);
				if (!Validation) return MakeRejected("The material graph is invalid.", std::move(Validation.Diagnostics));
			}
			Material->SetMaterialGraphPresentation(Candidate.Presentation);
		}
		if (Transactions)
		{
			const auto bRecorded = Transactions->CommitApplied(std::make_unique<FGraphDocumentChange>(
				*Owner.Get(), std::move(Before), std::move(Candidate), std::move(Description)));
			check(bRecorded);
		}
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphDocument::SetSignature(FMaterialFunctionSignature Signature,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (!State.bFunction) return MakeRejected("Only function documents have an interface.");
		State.Signature = std::move(Signature);
		for (auto& Node : State.Program.Nodes)
			if (Node.Opcode == EMaterialProgramOpcode::FunctionInput || Node.Opcode == EMaterialProgramOpcode::FunctionOutput)
			{
				const auto& Ports = Node.Opcode == EMaterialProgramOpcode::FunctionInput ? State.Signature.Inputs : State.Signature.Outputs;
				const auto Port = std::ranges::find(Ports, Node.FunctionPortId, &FMaterialFunctionPort::Id);
				if (Port != Ports.end()) Node.ResultType = Port->Type;
			}
		return Commit(std::move(State), "Edit Function Interface", Transactions);
	}

	auto FMaterialGraphDocument::AddPort(bool bOutput, FMaterialFunctionPort Port,
		FMaterialProgramLink Source, int32 X, int32 Y, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (!State.bFunction) return MakeRejected("Only function documents have an interface.");
		if (!Port.Id.IsValid()) Port.Id = FGuid::NewGuid();
		const auto NodeId = FGuid::NewGuid();
		FMaterialProgramNode Node{.Id = NodeId,
			.Opcode = bOutput ? EMaterialProgramOpcode::FunctionOutput : EMaterialProgramOpcode::FunctionInput,
			.ResultType = Port.Type, .FunctionPortId = Port.Id};
		if (bOutput) Node.Inputs.push_back(Source);
		(bOutput ? State.Signature.Outputs : State.Signature.Inputs).push_back(std::move(Port));
		State.Program.Nodes.push_back(std::move(Node));
		State.Presentation.Nodes.push_back({NodeId, X, Y});
		auto Result = Commit(std::move(State), bOutput ? "Add Function Output" : "Add Function Input", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {NodeId};
		return Result;
	}

	auto FMaterialGraphDocument::RemovePort(bool bOutput, const FGuid& PortId,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (!State.bFunction) return MakeRejected("Only function documents have an interface.");
		auto& Ports = bOutput ? State.Signature.Outputs : State.Signature.Inputs;
		if (!std::erase_if(Ports, [&](const auto& Port) { return Port.Id == PortId; }))
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		std::erase_if(State.Program.Nodes, [&](const auto& Node) { return Node.FunctionPortId == PortId; });
		return Commit(std::move(State), "Remove Function Port", Transactions);
	}

	auto FMaterialGraphDocument::CreateNode(FMaterialGraphCreateNodeRequest Request,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
		const auto Id = Request.Node.Id;
		State.Program.Nodes.push_back(std::move(Request.Node));
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y});
		auto Result = Commit(std::move(State), "Create Graph Node", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::CreateNodeWithDefaultInputs(FMaterialGraphCreateNodeRequest Request,
		FMaterialProgramLink FirstInput, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Signature = GetMaterialProgramNodeSignature(Request.Node.Opcode, Request.Node.ResultType);
		if (!Signature || Request.Node.ParameterId.IsValid()) return MakeRejected("This node is not available in a function graph.");
		if (!State.bFunction)
		{
			std::vector<std::vector<EMaterialProgramValueType>> Types;
			Request.Node.Inputs.resize(Signature->InputCount);
			if (!Request.Node.Inputs.empty()) Request.Node.Inputs[0] = FirstInput;
			for (uint32 Index = 0; Index < Signature->InputCount; ++Index)
				Types.emplace_back(Signature->Inputs[Index].begin(), Signature->Inputs[Index].end());
			return FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Cast<DMaterial>(Owner.Get()), Request, Types, Transactions);
		}
		Request.Node.Id = FGuid::NewGuid();
		Request.Node.Inputs.clear();
		std::vector<FGuid> Generated{Request.Node.Id};
		for (uint32 Index = 0; Index < Signature->InputCount; ++Index)
		{
			if (Index == 0 && FirstInput.SourceNodeId.IsValid()) { Request.Node.Inputs.push_back(FirstInput); continue; }
			const auto Type = Signature->Inputs[Index].front();
			if (Type >= EMaterialProgramValueType::Texture2D)
				return MakeRejected("Create this node by dragging from a compatible texture or Surface output.");
			FMaterialProgramNode Default{.Id = FGuid::NewGuid(), .ResultType = Type};
			if (Request.Node.Opcode == EMaterialProgramOpcode::MakeSurface)
				Default.Literal = GetMaterialSurfaceOutputDefault(State.Program.Outputs, static_cast<EMaterialSurfaceOutput>(Index));
			Request.Node.Inputs.push_back({Default.Id});
			Generated.push_back(Default.Id);
			State.Presentation.Nodes.push_back({Default.Id, Request.X - 320, Request.Y + static_cast<int32>(Index) * 140});
			State.Program.Nodes.push_back(std::move(Default));
		}
		State.Presentation.Nodes.push_back({Request.Node.Id, Request.X, Request.Y});
		State.Program.Nodes.push_back(std::move(Request.Node));
		auto Result = Commit(std::move(State), "Create Graph Node", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = std::move(Generated);
		return Result;
	}

	auto FMaterialGraphDocument::ReplaceNode(FMaterialProgramNode Node,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Existing = FindNode(State.Program, Node.Id);
		if (!Existing) return MakeRejected("The graph node no longer exists.");
		const auto Id = Node.Id;
		*Existing = std::move(Node);
		auto Result = Commit(std::move(State), "Edit Graph Node", Transactions);
		if (Result) Result.AffectedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::RemoveNodes(std::span<const FGuid> NodeIds,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		if (NodeIds.size() > MaterialProgramMaxNodeCount) return MakeRejected("The removal selection exceeds the graph bound.");
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		if (State.bFunction)
			for (const auto& Node : State.Program.Nodes)
				if (Removed.contains(Node.Id) && Node.FunctionPortId.IsValid())
				{
					auto& Ports = Node.Opcode == EMaterialProgramOpcode::FunctionOutput ? State.Signature.Outputs : State.Signature.Inputs;
					std::erase_if(Ports, [&](const auto& Port) { return Port.Id == Node.FunctionPortId; });
				}
		std::erase_if(State.Program.Nodes, [&](const auto& Node) { return Removed.contains(Node.Id); });
		std::erase_if(State.Calls, [&](const auto& Call) { return Removed.contains(Call.NodeId); });
		for (auto& Call : State.Calls)
			std::erase_if(Call.Inputs, [&](const auto& Binding) { return Removed.contains(Binding.Source.SourceNodeId); });
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			auto& Link = GetMaterialSurfaceOutputLink(State.Program.Outputs, static_cast<EMaterialSurfaceOutput>(Index));
			if (Removed.contains(Link.SourceNodeId)) Link = {};
		}
		if (Removed.contains(State.Program.Outputs.Surface.SourceNodeId)) State.Program.Outputs.Surface = {};
		auto Result = Commit(std::move(State), "Remove Graph Nodes", Transactions);
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
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Node = FindNode(State.Program, NodeId);
		if (!Node) return MakeRejected("The graph input no longer exists.");
		FMaterialProgramLink* Target = nullptr;
		if (Node->Opcode == EMaterialProgramOpcode::SetSurfaceAttributes && InputIndex > 0)
		{
			for (auto& Attribute : Node->SurfaceAttributes)
				if (static_cast<uint32>(Attribute.Attribute) + 1 == InputIndex) Target = &Attribute.Source;
		}
		else if (InputIndex < Node->Inputs.size()) Target = &Node->Inputs[InputIndex];
		if (!Target) return MakeRejected("The graph input no longer exists.");
		auto& Input = *Target;
		if (Input.SourceNodeId.IsValid() && Input != Source && !bReplaceExisting)
			return MakeRejected("The graph input is already connected.");
		Input = Source;
		return Commit(std::move(State), "Connect Graph Input", Transactions);
	}

	auto FMaterialGraphDocument::AssignMaterialOutput(std::optional<EMaterialSurfaceOutput> Attribute,
		FMaterialProgramLink Source, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (State.bFunction) return MakeRejected("Function documents expose output ports instead of Material Output.");
		if (Attribute)
		{
			if (static_cast<uint32>(*Attribute) >= 8) return MakeRejected("The material output attribute is invalid.");
			GetMaterialSurfaceOutputLink(State.Program.Outputs, *Attribute) = Source;
			if (Source.SourceNodeId.IsValid()) State.Program.Outputs.Surface = {};
		}
		else
		{
			State.Program.Outputs.Surface = Source;
			if (Source.SourceNodeId.IsValid())
				for (uint32 Index = 0; Index < 8; ++Index)
					GetMaterialSurfaceOutputLink(State.Program.Outputs, static_cast<EMaterialSurfaceOutput>(Index)) = {};
		}
		return Commit(std::move(State), "Connect Material Output", Transactions);
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
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		FMaterialFunctionClosure Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = SnapshotMaterialFunctionClosure(Roots, Closure);
		if (!Validation) return MakeRejected("The selected function dependency is invalid.", std::move(Validation.Diagnostics));
		if (std::ranges::any_of(Closure.Functions, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
			return MakeRejected("This call would introduce recursive function dependencies.");
		const FGuid Id = FGuid::NewGuid();
		State.Program.Nodes.push_back({.Id = Id, .Opcode = EMaterialProgramOpcode::FunctionCall});
		FMaterialFunctionCall Call{.NodeId = Id, .Function = &Function};
		Call.Inputs.assign(Inputs.begin(), Inputs.end());
		for (const auto& Output : Function.GetFunctionSignature().Outputs) Call.Outputs.push_back({Output.Id, Output.Type});
		Validation = ValidateMaterialFunctionCallSignature({Call.NodeId, Function.GetObjectPath(), Call.Inputs, Call.Outputs}, Function.GetFunctionSignature());
		if (!Validation) return MakeRejected("Bind the function's required inputs before inserting the call.", std::move(Validation.Diagnostics));
		State.Calls.push_back(std::move(Call));
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = Commit(std::move(State), "Insert Function Call", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto FMaterialGraphDocument::ConnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
		FMaterialProgramLink Source, bool bReplaceExisting, DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto Call = std::ranges::find(State.Calls, CallNodeId, &FMaterialFunctionCall::NodeId);
		if (Call == State.Calls.end() || !IsValid(Call->Function.Get())) return MakeRejected("The function call is unavailable.");
		const auto& Inputs = Call->Function->GetFunctionSignature().Inputs;
		const auto Port = std::ranges::find(Inputs, InputId, &FMaterialFunctionPort::Id);
		if (Port == Inputs.end()) return MakeRejected("The function input port no longer exists.");
		auto Binding = std::ranges::find(Call->Inputs, InputId, &FMaterialFunctionInputBinding::InputId);
		if (Binding != Call->Inputs.end())
		{
			if (!bReplaceExisting && Binding->Source != Source) return MakeRejected("The function input is already connected.");
			*Binding = {InputId, Port->Type, Source};
		}
		else Call->Inputs.push_back({InputId, Port->Type, Source});
		return Commit(std::move(State), "Connect Function Input", Transactions);
	}

	auto FMaterialGraphDocument::DisconnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
		DTransactor* Transactions) const -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto Call = std::ranges::find(State.Calls, CallNodeId, &FMaterialFunctionCall::NodeId);
		if (Call == State.Calls.end()) return MakeRejected("The function call no longer exists.");
		std::erase_if(Call->Inputs, [&](const auto& Binding) { return Binding.InputId == InputId; });
		return Commit(std::move(State), "Disconnect Function Input", Transactions);
	}
}
