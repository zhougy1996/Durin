#include "Materials/MaterialFunctionTypes.h"
#include "MaterialProgramValidation.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto IsType(EMaterialProgramValueType Type) -> bool
			{ return Type <= EMaterialProgramValueType::Surface; }
		auto IsFinite(const FMaterialProgramLiteral& Value) -> bool
			{ return std::isfinite(Value.X) && std::isfinite(Value.Y)
				&& std::isfinite(Value.Z) && std::isfinite(Value.W); }
		auto IsDisconnected(const FMaterialProgramLink& Link) -> bool
			{ return !Link.SourceNodeId.IsValid() && !Link.SourceOutputId.IsValid()
				&& Link.SourceOutputIndex == 0; }
		auto Error(FMaterialProgramValidationResult& Result, std::string Message,
			FGuid NodeId = {}, FGuid PortId = {},
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> void
		{
			Result.bSucceeded = false;
			if (Result.Diagnostics.size() == MaterialProgramMaxDiagnosticCount) return;
			Message.resize(std::min<size_t>(Message.size(), MaterialProgramMaxDiagnosticMessageBytes));
			Result.Diagnostics.push_back({.Category = Category,
				.LocationKind = NodeId.IsValid() ? EMaterialProgramDiagnosticLocationKind::Node
					: EMaterialProgramDiagnosticLocationKind::Program,
				.NodeId = NodeId, .Message = std::move(Message), .PortId = PortId});
		}
	}

	auto ValidateMaterialFunctionSignature(const FMaterialFunctionSignature& Signature)
		-> FMaterialProgramValidationResult
	{
		FMaterialProgramValidationResult Result;
		if (Signature.Inputs.size() > MaterialFunctionMaxInputs || Signature.Outputs.empty()
			|| Signature.Outputs.size() > MaterialFunctionMaxOutputs)
		{
			Error(Result, "Function signature exceeds input/output bounds.", {}, {},
				EMaterialProgramDiagnosticCategory::Bounds);
			return Result;
		}
		std::unordered_set<FGuid> Ids;
		std::unordered_map<FGuid, size_t> InputIndices;
		for (size_t Index = 0; Index < Signature.Inputs.size(); ++Index)
			InputIndices.emplace(Signature.Inputs[Index].Id, Index);
		for (bool bInput : {true, false})
			for (const auto& Port : bInput ? Signature.Inputs : Signature.Outputs)
			{
				if (!Port.Id.IsValid() || !Ids.emplace(Port.Id).second)
					Error(Result, "Function port GUID is missing or duplicated.", {}, Port.Id);
				if (!IsType(Port.Type) || Port.Name.empty()
					|| Port.Name.size() > MaterialProgramMaxDisplayNameBytes)
					Error(Result, "Function port type or name is invalid.", {}, Port.Id);
				const auto& Default = Port.Default;
				if (!bInput || Port.bRequired)
				{
					if (Default.Kind != EMaterialFunctionDefaultKind::None || (!bInput && Port.bRequired))
						Error(Result, "Outputs and required inputs cannot declare defaults.", {}, Port.Id);
					continue;
				}
				bool bValid = false;
				switch (Default.Kind)
				{
				case EMaterialFunctionDefaultKind::Numeric:
					bValid = Port.Type <= EMaterialProgramValueType::Float4 && IsFinite(Default.Numeric);
					break;
				case EMaterialFunctionDefaultKind::Texture:
					bValid = Port.Type == EMaterialProgramValueType::Texture2D
						&& IsValidMaterialSampling(Default.Sampler, Default.TextureFallback);
					break;
				case EMaterialFunctionDefaultKind::Surface:
					bValid = Port.Type == EMaterialProgramValueType::Surface
						&& IsDisconnected(Default.Surface.Surface);
					for (uint8 Index = 0; Index < 8; ++Index)
					{
						const auto Output = static_cast<EMaterialSurfaceOutput>(Index);
						bValid &= IsDisconnected(GetMaterialSurfaceOutputLink(Default.Surface, Output))
							&& IsFinite(GetMaterialSurfaceOutputDefault(Default.Surface, Output));
					}
					break;
				case EMaterialFunctionDefaultKind::Input:
					if (const auto It = InputIndices.find(Default.InputId); It != InputIndices.end())
						bValid = Signature.Inputs[It->second].Type == Port.Type;
					break;
				case EMaterialFunctionDefaultKind::UV0:
					bValid = Port.Type == EMaterialProgramValueType::Float2;
					break;
				default: break;
				}
				if (!bValid) Error(Result, "Optional function input has a missing or incompatible default.", {}, Port.Id);
			}
		std::vector<uint8> State(Signature.Inputs.size());
		const auto Visit = [&](auto&& Self, size_t Index) -> void {
			const auto& Port = Signature.Inputs[Index];
			if (State[Index] == 2) return;
			if (State[Index] == 1)
			{
				Error(Result, "Function input defaults contain a cycle.", {}, Port.Id);
				return;
			}
			State[Index] = 1;
			if (!Port.bRequired && Port.Default.Kind == EMaterialFunctionDefaultKind::Input)
				if (const auto It = InputIndices.find(Port.Default.InputId); It != InputIndices.end())
					Self(Self, It->second);
			State[Index] = 2;
		};
		for (size_t Index = 0; Index < State.size(); ++Index) Visit(Visit, Index);
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}

	auto ValidateMaterialFunctionCallSignature(const FMaterialFunctionCallSnapshot& Call,
		const FMaterialFunctionSignature& Signature) -> FMaterialProgramValidationResult
	{
		auto Result = ValidateMaterialFunctionSignature(Signature);
		if (!Result) return Result;
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.size() > MaterialFunctionMaxOutputs)
		{
			Error(Result, "Function call exceeds port bounds.", Call.NodeId, {}, EMaterialProgramDiagnosticCategory::Bounds);
			return Result;
		}
		std::unordered_set<FGuid> BoundInputs, BoundOutputs;
		for (const auto& Binding : Call.Inputs)
		{
			const auto Port = std::ranges::find(Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
			if (Port == Signature.Inputs.end() || Port->Type != Binding.ExpectedType
				|| !BoundInputs.emplace(Binding.InputId).second)
				Error(Result, "Function input was removed, retyped or bound more than once.", Call.NodeId, Binding.InputId);
		}
		for (const auto& Binding : Call.Outputs)
		{
			const auto Port = std::ranges::find(Signature.Outputs, Binding.OutputId, &FMaterialFunctionPort::Id);
			if (Port == Signature.Outputs.end() || Port->Type != Binding.ExpectedType
				|| !BoundOutputs.emplace(Binding.OutputId).second)
				Error(Result, "Function output was removed, retyped or recorded more than once.", Call.NodeId, Binding.OutputId);
		}
		for (const auto& Port : Signature.Inputs)
			if (Port.bRequired && !BoundInputs.contains(Port.Id))
				Error(Result, "Required function input is not connected.", Call.NodeId, Port.Id);
		for (auto& Diagnostic : Result.Diagnostics) Diagnostic.FunctionAssetPath = Call.FunctionPath;
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}

	auto ValidateMaterialFunctionGraph(const FMaterialFunctionGraph& Graph)
		-> FMaterialProgramValidationResult
	{
		auto Result = ValidateMaterialFunctionSignature(Graph.Signature);
		if (Graph.SchemaVersion != CurrentMaterialFunctionSchemaVersion)
			Error(Result, "Function graph schema is unsupported.", {}, {}, EMaterialProgramDiagnosticCategory::Schema);
		if (Graph.Nodes.size() > MaterialProgramMaxNodeCount || Graph.Calls.size() > Graph.Nodes.size())
			Error(Result, "Function graph exceeds the node/call bound.", {}, {}, EMaterialProgramDiagnosticCategory::Bounds);
		if (!Result.Diagnostics.empty()) return Result;
		std::unordered_map<FGuid, size_t> Nodes;
		std::unordered_map<FGuid, const FMaterialFunctionCall*> Calls;
		uint64 Links = 0, Bytes = 0, Strings = 0;
		for (size_t Index = 0; Index < Graph.Nodes.size(); ++Index)
		{
			const auto& Node = Graph.Nodes[Index];
			if (!Node.Id.IsValid() || !Nodes.emplace(Node.Id, Index).second)
				Error(Result, "Function node GUID is missing or duplicated.", Node.Id);
			Links += Node.Inputs.size() + Node.SurfaceAttributes.size();
			Strings += Node.DisplayName.size();
			Bytes += sizeof(Node) + Node.DisplayName.size() + Node.Inputs.size() * sizeof(FMaterialProgramLink)
				+ Node.SurfaceAttributes.size() * sizeof(FMaterialSurfaceAttributeBinding);
			if (Node.Inputs.size() > MaterialProgramMaxNodeInputCount || Node.SurfaceAttributes.size() > 8 || !IsType(Node.ResultType)
				|| Node.DisplayName.size() > MaterialProgramMaxDisplayNameBytes)
				Error(Result, "Function node type, input count or name is invalid.", Node.Id);
			if (Node.ParameterId.IsValid())
				Error(Result, "Function nodes cannot reference root material parameter identities.", Node.Id);
		}
		for (const auto& Call : Graph.Calls)
		{
			const auto Node = Nodes.find(Call.NodeId);
			if (Node == Nodes.end() || Graph.Nodes[Node->second].Opcode != EMaterialProgramOpcode::FunctionCall
				|| !Calls.emplace(Call.NodeId, &Call).second)
				Error(Result, "Function call record is duplicated or has no call node.", Call.NodeId);
			if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.empty()
				|| Call.Outputs.size() > MaterialFunctionMaxOutputs)
			{
				Error(Result, "Function call exceeds port bounds.", Call.NodeId, {}, EMaterialProgramDiagnosticCategory::Bounds);
				continue;
			}
			Links += Call.Inputs.size();
			Bytes += sizeof(Call) + Call.Inputs.size() * sizeof(FMaterialFunctionInputBinding)
				+ Call.Outputs.size() * sizeof(FMaterialFunctionOutputBinding);
			std::unordered_set<FGuid> Ports;
			for (const auto& Input : Call.Inputs)
				if (!Input.InputId.IsValid() || !Ports.emplace(Input.InputId).second || !IsType(Input.ExpectedType))
					Error(Result, "Function call input binding is invalid or duplicated.", Call.NodeId, Input.InputId);
			for (const auto& Output : Call.Outputs)
				if (!Output.OutputId.IsValid() || !Ports.emplace(Output.OutputId).second || !IsType(Output.ExpectedType))
					Error(Result, "Function call output binding is invalid or duplicated.", Call.NodeId, Output.OutputId);
		}
		for (const auto& Port : Graph.Signature.Inputs)
			{ Bytes += sizeof(Port) + Port.Name.size(); Strings += Port.Name.size(); }
		for (const auto& Port : Graph.Signature.Outputs)
			{ Bytes += sizeof(Port) + Port.Name.size(); Strings += Port.Name.size(); }
		if (Links > MaterialProgramMaxLinkCount || Bytes > MaterialProgramMaxCanonicalBytes
			|| Strings > MaterialProgramMaxStringBytes)
			Error(Result, "Function graph exceeds link, string or payload bounds.", {}, {}, EMaterialProgramDiagnosticCategory::Bounds);
		if (!Result.Diagnostics.empty()) { Result.bSucceeded = false; return Result; }
		const auto LinkType = [&](const FMaterialProgramLink& Link) -> std::optional<EMaterialProgramValueType> {
			const auto It = Nodes.find(Link.SourceNodeId);
			if (It == Nodes.end()) return {};
			const auto& Node = Graph.Nodes[It->second];
			if (Node.Opcode == EMaterialProgramOpcode::FunctionCall)
			{
				const auto Call = Calls.find(Node.Id);
				if (Call == Calls.end() || !Link.SourceOutputId.IsValid() || Link.SourceOutputIndex != 0) return {};
				for (const auto& Output : Call->second->Outputs)
					if (Output.OutputId == Link.SourceOutputId) return Output.ExpectedType;
				return {};
			}
			return Private::ResolveMaterialBuiltinOutput(Node, Link);
		};
		std::unordered_set<FGuid> Terminals;
		for (const auto& Node : Graph.Nodes)
		{
			std::vector<EMaterialProgramValueType> InputTypes;
			for (auto& Diagnostic : Private::ValidateMaterialSurfacePayload(Node, LinkType).Diagnostics)
				if (Result.Diagnostics.size() < MaterialProgramMaxDiagnosticCount) Result.Diagnostics.push_back(std::move(Diagnostic));
			for (const auto& Link : Node.Inputs)
			{
				const auto Type = LinkType(Link);
				if (!Type) Error(Result, "Function link has no matching source output.", Node.Id);
				InputTypes.push_back(Type.value_or(EMaterialProgramValueType::Float));
			}
			if (Node.Opcode == EMaterialProgramOpcode::FunctionInput || Node.Opcode == EMaterialProgramOpcode::FunctionOutput)
			{
				const bool bInput = Node.Opcode == EMaterialProgramOpcode::FunctionInput;
				const auto& Ports = bInput ? Graph.Signature.Inputs : Graph.Signature.Outputs;
				const auto Port = std::ranges::find(Ports, Node.FunctionPortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end() || !Terminals.emplace(Node.FunctionPortId).second
					|| Port->Type != Node.ResultType || Node.Inputs.size() != (bInput ? 0u : 1u)
					|| (!bInput && !InputTypes.empty() && InputTypes[0] != Node.ResultType))
					Error(Result, "Function terminal does not match its unique typed declaration.", Node.Id, Node.FunctionPortId);
			}
			else if (Node.Opcode == EMaterialProgramOpcode::FunctionCall)
			{
				const auto Call = Calls.find(Node.Id);
				if (Call == Calls.end() || !Node.Inputs.empty() || Node.FunctionPortId.IsValid())
					Error(Result, "Function call requires a call record and stable port bindings.", Node.Id);
				else for (const auto& Binding : Call->second->Inputs)
					if (LinkType(Binding.Source) != Binding.ExpectedType)
						Error(Result, "Function call input source has an incompatible type.", Node.Id, Binding.InputId);
			}
			else
			{
				if (Node.Opcode == EMaterialProgramOpcode::Parameter || Node.Opcode == EMaterialProgramOpcode::TextureParameter
					|| Node.FunctionPortId.IsValid())
					Error(Result, "Functions cannot declare material parameters or attach ports to built-in nodes.", Node.Id);
				else
				{
					auto Validation = Private::ValidateMaterialBuiltinNode(Node, InputTypes);
					for (auto& Diagnostic : Validation.Diagnostics)
						if (Result.Diagnostics.size() < MaterialProgramMaxDiagnosticCount)
							Result.Diagnostics.push_back(std::move(Diagnostic));
				}
			}
		}
		for (const auto& Output : Graph.Signature.Outputs)
			if (!Terminals.contains(Output.Id)) Error(Result, "Function output has no terminal.", {}, Output.Id);
		std::vector<uint8> State(Graph.Nodes.size());
		std::vector<uint32> Depth(Graph.Nodes.size());
		const auto Visit = [&](auto&& Self, size_t Index) -> uint32 {
			if (State[Index] == 2) return Depth[Index];
			const auto& Node = Graph.Nodes[Index];
			if (State[Index] == 1) { Error(Result, "Function graph contains a cycle.", Node.Id); return 0; }
			State[Index] = 1;
			uint32 MaximumDepth = 0;
			const auto Follow = [&](const FMaterialProgramLink& Link) {
				if (const auto It = Nodes.find(Link.SourceNodeId); It != Nodes.end())
					MaximumDepth = std::max(MaximumDepth, Self(Self, It->second));
			};
			for (const auto& Link : Node.Inputs) Follow(Link);
			for (const auto& Binding : Node.SurfaceAttributes) Follow(Binding.Source);
			if (const auto Call = Calls.find(Node.Id); Call != Calls.end())
				for (const auto& Binding : Call->second->Inputs) Follow(Binding.Source);
			State[Index] = 2;
			Depth[Index] = MaximumDepth + 1;
			if (Depth[Index] > MaterialProgramMaxDepth)
				Error(Result, "Function graph exceeds expression depth.", Node.Id, {}, EMaterialProgramDiagnosticCategory::Bounds);
			return Depth[Index];
		};
		for (size_t Index = 0; Index < Graph.Nodes.size(); ++Index) Visit(Visit, Index);
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}
}
