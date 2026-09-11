#include "MaterialFunctionExpansion.h"

#include <unordered_set>

namespace Durin::Private
{
	namespace
	{
		struct FExpandedValue
		{
			FMaterialProgramLink Link;
			bool bDefaultTexture = false;
			FMaterialSamplerState Sampler;
			EMaterialTextureFallback Fallback = EMaterialTextureFallback::White;
		};

		struct FInvocation
		{
			std::span<const FMaterialProgramNode> Nodes;
			std::span<const FMaterialFunctionCallSnapshot> Calls;
			const FMaterialFunctionSnapshot* Function = nullptr;
			std::vector<FGuid> Path;
			std::unordered_map<FGuid, FExpandedValue> Inputs;
			std::unordered_map<FGuid, FExpandedValue> Values;
			std::unordered_map<FGuid, std::unordered_map<FGuid, FExpandedValue>> Outputs;
			std::unordered_set<FGuid> ActiveNodes;
			std::unordered_set<FGuid> ActiveCalls;
			uint32 SyntheticIndex = 0;
		};

		class FExpander
		{
		public:
			FMaterialExpandedProgram Program;
			FMaterialProgramValidationResult Result;
			std::unordered_map<std::string, const FMaterialFunctionSnapshot*> Functions;
			std::vector<std::string> ActiveFunctions;
			uint64 NodeWeight = 0;
			uint64 LinkWeight = 0;

			auto Fail(const FInvocation& Context, FGuid NodeId, std::string Message,
				EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph,
				FGuid PortId = {}) -> void
			{
				if (Result.Diagnostics.size() == MaterialProgramMaxDiagnosticCount) return;
				Result.Diagnostics.push_back({.Category = Category,
					.LocationKind = EMaterialProgramDiagnosticLocationKind::Node, .NodeId = NodeId,
					.Message = std::move(Message), .PortId = PortId,
					.FunctionAssetPath = Context.Function ? Context.Function->AssetPath : std::string{}, .CallPath = Context.Path});
			}

			auto Admit(FInvocation& Context) -> bool
			{
				NodeWeight += Context.Nodes.size();
				for (const auto& Node : Context.Nodes) LinkWeight += Node.Inputs.size() + Node.SurfaceAttributes.size();
				for (const auto& Call : Context.Calls) LinkWeight += Call.Inputs.size();
				return CheckBounds(Context);
			}

			auto CheckBounds(const FInvocation& Context) -> bool
			{
				if (NodeWeight <= MaterialFunctionMaxExpandedNodes && LinkWeight <= MaterialFunctionMaxExpandedLinks)
					return true;
				Fail(Context, {}, "Function expansion exceeds node or link bounds before pruning.", EMaterialProgramDiagnosticCategory::Bounds);
				return false;
			}

			auto Emit(FInvocation& Context, FMaterialProgramNode Node, FGuid Origin,
				bool bSynthetic = false, FGuid PortId = {}) -> FExpandedValue
			{
				if (bSynthetic) { ++NodeWeight; LinkWeight += Node.Inputs.size(); }
				if (!CheckBounds(Context)) return {};
				std::vector<FGuid> Namespace = Context.Path;
				Namespace.push_back(Origin);
				Namespace.push_back({0x45787061, 0x6e73696f, 0x6e4e6f64, bSynthetic ? ++Context.SyntheticIndex : 0});
				const auto Hash = FXxHash128::HashBuffer(std::as_bytes(std::span(Namespace)));
				Node.Id = {static_cast<uint32>(Hash.HashLow), static_cast<uint32>(Hash.HashLow >> 32),
					static_cast<uint32>(Hash.HashHigh), static_cast<uint32>(Hash.HashHigh >> 32)};
				Node.FunctionPortId = {};
				Node.SurfaceAttributeMask = 0;
				Node.SurfaceAttributes.clear();
				Node.DisplayName.clear();
				if (!Node.Id.IsValid() || Program.Sources.contains(Node.Id))
				{
					Fail(Context, Origin, "Function expansion node namespace collided.");
					return {};
				}
				Program.Sources.emplace(Node.Id, FMaterialExpressionSource{.NodeId = Origin, .PortId = PortId,
					.FunctionAssetPath = Context.Function ? Context.Function->AssetPath : std::string{}, .CallPath = Context.Path});
				const FExpandedValue Value{.Link = {.SourceNodeId = Node.Id}};
				Program.Nodes.push_back(std::move(Node));
				return Value;
			}

			auto Literal(FInvocation& Context, EMaterialProgramValueType Type, FMaterialProgramLiteral Value,
				FGuid PortId) -> FExpandedValue
			{
				return Emit(Context, {.Opcode = EMaterialProgramOpcode::Constant, .ResultType = Type, .Literal = Value},
				{}, true, PortId);
			}

			auto Input(FInvocation& Context, const FGuid& PortId) -> FExpandedValue
			{
				if (const auto It = Context.Inputs.find(PortId); It != Context.Inputs.end()) return It->second;
				if (!Context.Function) { Fail(Context, {}, "Function input appeared in a root material."); return {}; }
				const auto Port = std::ranges::find(Context.Function->Signature.Inputs, PortId, &FMaterialFunctionPort::Id);
				if (Port == Context.Function->Signature.Inputs.end() || Port->bRequired)
				{ Fail(Context, {}, "Required function input has no binding.", EMaterialProgramDiagnosticCategory::Type, PortId); return {}; }
				FExpandedValue Value;
				const auto& Default = Port->Default;
				switch (Default.Kind)
				{
				case EMaterialFunctionDefaultKind::Input:
					Value = Input(Context, Default.InputId);
					break;
				case EMaterialFunctionDefaultKind::Numeric:
					Value = Literal(Context, Port->Type, Default.Numeric, PortId);
					break;
				case EMaterialFunctionDefaultKind::Texture:
					Value = {.bDefaultTexture = true, .Sampler = Default.Sampler, .Fallback = Default.TextureFallback};
					break;
				case EMaterialFunctionDefaultKind::UV0:
				{
					const auto Channel = Literal(Context, EMaterialProgramValueType::Float, {}, PortId);
					Value = Emit(Context, {.Opcode = EMaterialProgramOpcode::UVChannel,
						.ResultType = EMaterialProgramValueType::Float2, .Inputs = {Channel.Link}}, {}, true, PortId);
					break;
				}
				case EMaterialFunctionDefaultKind::Surface:
				{
					FMaterialProgramNode Surface{.Opcode = EMaterialProgramOpcode::MakeSurface,
						.ResultType = EMaterialProgramValueType::Surface};
					for (uint8 Index = 0; Index < 8; ++Index)
					{
						const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
						Surface.Inputs.push_back(Literal(Context, GetMaterialSurfaceOutputType(Attribute),
							GetMaterialSurfaceOutputDefault(Default.Surface, Attribute), PortId).Link);
					}
					Value = Emit(Context, std::move(Surface), {}, true, PortId);
					break;
				}
				default: Fail(Context, {}, "Function input default is invalid.", EMaterialProgramDiagnosticCategory::Type, PortId); break;
				}
				Context.Inputs.emplace(PortId, Value);
				return Value;
			}

			auto Call(FInvocation& Context, FGuid NodeId) -> bool
			{
				if (Context.Outputs.contains(NodeId)) return true;
				if (!Context.ActiveCalls.emplace(NodeId).second)
				{ Fail(Context, NodeId, "Function call bindings contain a cycle."); return false; }
				const auto Record = std::ranges::find(Context.Calls, NodeId, &FMaterialFunctionCallSnapshot::NodeId);
				if (Record == Context.Calls.end()) { Fail(Context, NodeId, "Function call record is missing."); return false; }
				const auto Function = Functions.find(Record->FunctionPath);
				if (Function == Functions.end()) { Fail(Context, NodeId, "Function is missing from the detached closure.", EMaterialProgramDiagnosticCategory::Dependency); return false; }
				if (Context.Path.size() >= MaterialFunctionMaxCallDepth
					|| std::ranges::find(ActiveFunctions, Record->FunctionPath) != ActiveFunctions.end())
				{ Fail(Context, NodeId, "Function expansion contains recursion or exceeds call depth.", EMaterialProgramDiagnosticCategory::Bounds); return false; }
				const auto& Snapshot = *Function->second;
				FInvocation Child{.Nodes = Snapshot.Nodes, .Calls = Snapshot.Calls, .Function = &Snapshot, .Path = Context.Path};
				Child.Path.push_back(NodeId);
				for (const auto& Binding : Record->Inputs) Child.Inputs.emplace(Binding.InputId, Link(Context, Binding.Source));
				if (!Result.Diagnostics.empty() || !Admit(Child)) return false;
				ActiveFunctions.push_back(Record->FunctionPath);
				if (!All(Child)) return false;
				std::unordered_map<FGuid, FExpandedValue> Outputs;
				for (const auto& Node : Snapshot.Nodes)
					if (Node.Opcode == EMaterialProgramOpcode::FunctionOutput)
						Outputs.emplace(Node.FunctionPortId, Value(Child, Node.Id));
				ActiveFunctions.pop_back();
				Context.Outputs.emplace(NodeId, std::move(Outputs));
				Context.ActiveCalls.erase(NodeId);
				return Result.Diagnostics.empty();
			}

			auto Link(FInvocation& Context, const FMaterialProgramLink& Source) -> FExpandedValue
			{
				const auto Node = std::ranges::find(Context.Nodes, Source.SourceNodeId, &FMaterialProgramNode::Id);
				if (Node == Context.Nodes.end()) { Fail(Context, Source.SourceNodeId, "Function expansion encountered a dangling link."); return {}; }
				if (Node->Opcode == EMaterialProgramOpcode::FunctionCall)
				{
					if (!Call(Context, Node->Id)) return {};
					const auto& Outputs = Context.Outputs.at(Node->Id);
					const auto Output = Outputs.find(Source.SourceOutputId);
					if (Output == Outputs.end()) { Fail(Context, Node->Id, "Function output terminal is missing."); return {}; }
					return Output->second;
				}
				const auto Expanded = Value(Context, Source.SourceNodeId);
				if (Node->Opcode == EMaterialProgramOpcode::GetSurfaceAttributes)
				{
					const auto Surface = std::ranges::find(Program.Nodes, Expanded.Link.SourceNodeId, &FMaterialProgramNode::Id);
					if (Surface == Program.Nodes.end() || Surface->Opcode != EMaterialProgramOpcode::MakeSurface
						|| Source.SourceOutputIndex >= Surface->Inputs.size())
					{ Fail(Context, Node->Id, "Surface attribute source could not be lowered."); return {}; }
					return {.Link = Surface->Inputs[Source.SourceOutputIndex]};
				}
				return Expanded;
			}

			auto Value(FInvocation& Context, FGuid NodeId) -> FExpandedValue
			{
				if (const auto It = Context.Values.find(NodeId); It != Context.Values.end()) return It->second;
				if (Context.ActiveNodes.size() >= MaterialProgramMaxDepth || !Context.ActiveNodes.emplace(NodeId).second)
				{ Fail(Context, NodeId, "Function expression is cyclic or exceeds depth.", EMaterialProgramDiagnosticCategory::Bounds); return {}; }
				const auto Node = std::ranges::find(Context.Nodes, NodeId, &FMaterialProgramNode::Id);
				if (Node == Context.Nodes.end()) { Fail(Context, NodeId, "Function node is missing."); return {}; }
				FExpandedValue Expanded;
				if (Node->Opcode == EMaterialProgramOpcode::FunctionInput) Expanded = Input(Context, Node->FunctionPortId);
				else if (Node->Opcode == EMaterialProgramOpcode::FunctionOutput) Expanded = Link(Context, Node->Inputs[0]);
				else if (Node->Opcode == EMaterialProgramOpcode::GetSurfaceAttributes) Expanded = Link(Context, Node->Inputs[0]);
				else if (Node->Opcode == EMaterialProgramOpcode::SetSurfaceAttributes)
				{
					const auto Base = Link(Context, Node->Inputs[0]);
					const auto Surface = std::ranges::find(Program.Nodes, Base.Link.SourceNodeId, &FMaterialProgramNode::Id);
					if (Surface == Program.Nodes.end() || Surface->Opcode != EMaterialProgramOpcode::MakeSurface)
					{ Fail(Context, NodeId, "Surface override base could not be lowered."); return {}; }
					FMaterialProgramNode Lowered{.Opcode = EMaterialProgramOpcode::MakeSurface,
						.ResultType = EMaterialProgramValueType::Surface, .Inputs = Surface->Inputs};
					for (const auto& Binding : Node->SurfaceAttributes)
						Lowered.Inputs[static_cast<uint8>(Binding.Attribute)] = Link(Context, Binding.Source).Link;
					if (!Result.Diagnostics.empty()) return {};
					Expanded = Emit(Context, std::move(Lowered), NodeId);
				}
				else
				{
					std::vector<FExpandedValue> Inputs;
					for (const auto& Source : Node->Inputs) Inputs.push_back(Link(Context, Source));
					if (!Result.Diagnostics.empty()) return {};
					auto Lowered = *Node;
					Lowered.Inputs.clear();
					if (Node->Opcode == EMaterialProgramOpcode::TextureSample2D && Inputs[0].bDefaultTexture)
					{
						Lowered.Opcode = EMaterialProgramOpcode::Constant;
						Lowered.Literal = Inputs[0].Fallback == EMaterialTextureFallback::White ? FMaterialProgramLiteral{1, 1, 1, 1}
							: Inputs[0].Fallback == EMaterialTextureFallback::FlatRGNormal ? FMaterialProgramLiteral{0.5f, 0.5f, 1, 1}
							: FMaterialProgramLiteral{0, 0, 0, 1};
					}
					else for (const auto& Input : Inputs)
					{
						if (Input.bDefaultTexture) { Fail(Context, NodeId, "Default texture is consumed by a non-sampling operation."); return {}; }
						Lowered.Inputs.push_back(Input.Link);
					}
					Expanded = Emit(Context, std::move(Lowered), NodeId);
				}
				Context.ActiveNodes.erase(NodeId);
				Context.Values.emplace(NodeId, Expanded);
				return Expanded;
			}

			auto All(FInvocation& Context) -> bool
			{
				for (const auto& Node : Context.Nodes)
				{
					if (Node.Opcode == EMaterialProgramOpcode::FunctionCall) Call(Context, Node.Id);
					else Value(Context, Node.Id);
					if (!Result.Diagnostics.empty()) return false;
				}
				return true;
			}
		};
	}

	auto ExpandMaterialFunctionCalls(const FMaterialCompilerInput& Input,
		std::span<const FMaterialParameterDefinition> Definitions,
		FMaterialExpandedProgram& OutProgram) -> FMaterialProgramValidationResult
	{
		FExpander Expander;
		Expander.Result = ValidateMaterialProgram(Input.Program, Definitions, Input.FunctionCalls);
		if (!Expander.Result) return Expander.Result;
		FInvocation Root{.Nodes = Input.Program.Nodes, .Calls = Input.FunctionCalls};
		if (Input.Functions.Functions.size() > MaterialFunctionMaxDependencies)
			Expander.Fail(Root, {}, "Detached closure exceeds dependency bounds.", EMaterialProgramDiagnosticCategory::Bounds);
		uint64 Bytes = 0;
		for (const auto& Function : Input.Functions.Functions)
		{
			if (!Expander.Result.Diagnostics.empty()) break;
			if (Function.Nodes.size() > MaterialProgramMaxNodeCount || Function.Calls.size() > Function.Nodes.size()
				|| Function.AssetPath.size() > MaterialProgramMaxStringBytes)
			{ Expander.Fail(Root, {}, "Detached function exceeds document bounds.", EMaterialProgramDiagnosticCategory::Bounds); break; }
			uint64 DocumentBytes = sizeof(Function) + Function.AssetPath.size();
			for (const auto& Node : Function.Nodes) DocumentBytes += sizeof(Node) + Node.DisplayName.size() + Node.Inputs.size() * sizeof(FMaterialProgramLink)
				+ Node.SurfaceAttributes.size() * sizeof(FMaterialSurfaceAttributeBinding);
			for (const auto& Call : Function.Calls) DocumentBytes += sizeof(Call) + Call.FunctionPath.size()
				+ Call.Inputs.size() * sizeof(FMaterialFunctionInputBinding) + Call.Outputs.size() * sizeof(FMaterialFunctionOutputBinding);
			for (const auto& Port : Function.Signature.Inputs) DocumentBytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Function.Signature.Outputs) DocumentBytes += sizeof(Port) + Port.Name.size();
			Bytes += DocumentBytes;
			if (DocumentBytes > MaterialProgramMaxCanonicalBytes || Bytes > MaterialFunctionMaxClosureBytes)
			{ Expander.Fail(Root, {}, "Detached function payload exceeds bounds.", EMaterialProgramDiagnosticCategory::Bounds); break; }
			FMaterialFunctionGraph Graph{.SchemaVersion = Function.SchemaVersion, .Signature = Function.Signature, .Nodes = Function.Nodes};
			for (const auto& Call : Function.Calls) Graph.Calls.push_back({.NodeId = Call.NodeId, .Inputs = Call.Inputs, .Outputs = Call.Outputs});
			Expander.Result = ValidateMaterialFunctionGraph(Graph);
			if (!Expander.Result)
			{
				for (auto& Diagnostic : Expander.Result.Diagnostics) Diagnostic.FunctionAssetPath = Function.AssetPath;
				break;
			}
			if (!Expander.Functions.emplace(Function.AssetPath, &Function).second)
				Expander.Fail(Root, {}, "Detached closure has duplicate function paths.");
		}
		const auto ValidateCalls = [&](std::span<const FMaterialFunctionCallSnapshot> Calls, const FInvocation& Context) {
			for (const auto& Call : Calls)
			{
				if (!Expander.Result.Diagnostics.empty()) return;
				const auto Target = Expander.Functions.find(Call.FunctionPath);
				if (Target == Expander.Functions.end()) { Expander.Fail(Context, Call.NodeId, "Detached function dependency is missing.", EMaterialProgramDiagnosticCategory::Dependency); return; }
				Expander.Result = ValidateMaterialFunctionCallSignature(Call, Target->second->Signature);
				for (auto& Diagnostic : Expander.Result.Diagnostics)
					Diagnostic.FunctionAssetPath = Context.Function ? Context.Function->AssetPath : std::string{};
			}
		};
		ValidateCalls(Input.FunctionCalls, Root);
		for (const auto& Function : Input.Functions.Functions) ValidateCalls(Function.Calls, {.Function = &Function});
		std::unordered_map<std::string, uint32> Heights;
		std::unordered_set<std::string> Visiting;
		const auto CheckClosure = [&](auto&& Self, const FMaterialFunctionSnapshot& Function, uint32 Depth) -> uint32 {
			if (!Expander.Result.Diagnostics.empty()) return 0;
			if (Depth > MaterialFunctionMaxCallDepth || Visiting.contains(Function.AssetPath))
			{
				Expander.Fail({.Function = &Function}, {}, "Detached closure contains recursion or exceeds call depth.",
					EMaterialProgramDiagnosticCategory::Bounds);
				return 0;
			}
			if (const auto Height = Heights.find(Function.AssetPath); Height != Heights.end())
			{
				if (Depth + Height->second - 1 > MaterialFunctionMaxCallDepth)
					Expander.Fail({.Function = &Function}, {}, "Detached shared subtree exceeds call depth.", EMaterialProgramDiagnosticCategory::Bounds);
				return Height->second;
			}
			Visiting.emplace(Function.AssetPath);
			uint32 Height = 1;
			for (const auto& Call : Function.Calls)
				Height = std::max(Height, 1 + Self(Self, *Expander.Functions.at(Call.FunctionPath), Depth + 1));
			Visiting.erase(Function.AssetPath);
			Heights.emplace(Function.AssetPath, Height);
			return Height;
		};
		for (const auto& Function : Input.Functions.Functions) CheckClosure(CheckClosure, Function, 1);
		if (Expander.Result.Diagnostics.empty() && Expander.Admit(Root) && Expander.All(Root))
		{
			Expander.Program.Outputs = Input.Program.Outputs;
			const auto LowerOutput = [&](FMaterialProgramLink& Source) {
				if (Source.SourceNodeId.IsValid()) Source = Expander.Link(Root, Source).Link;
			};
			LowerOutput(Expander.Program.Outputs.Surface);
			for (uint8 Index = 0; Index < 8; ++Index)
				LowerOutput(GetMaterialSurfaceOutputLink(Expander.Program.Outputs, static_cast<EMaterialSurfaceOutput>(Index)));
			if (Expander.Result.Diagnostics.empty())
			{
				Expander.Result = ValidateMaterialProgramGraph({Expander.Program.SchemaVersion, Expander.Program.Nodes, Expander.Program.Outputs}, Definitions, {}, true);
				for (auto& Diagnostic : Expander.Result.Diagnostics)
					if (const auto Source = Expander.Program.Sources.find(Diagnostic.NodeId); Source != Expander.Program.Sources.end())
					{
						Diagnostic.NodeId = Source->second.NodeId;
						Diagnostic.PortId = Source->second.PortId;
						Diagnostic.FunctionAssetPath = Source->second.FunctionAssetPath;
						Diagnostic.CallPath = Source->second.CallPath;
					}
			}
		}
		Expander.Result.bSucceeded = Expander.Result.Diagnostics.empty();
		// Validation can fail before invocation expansion. Recover the authored path
		// from detached records so those diagnostics remain navigable too.
		for (auto& Diagnostic : Expander.Result.Diagnostics)
		{
			if (Diagnostic.FunctionAssetPath.empty() || !Diagnostic.CallPath.empty()) continue;
			std::vector<FGuid> Path;
			std::unordered_set<std::string> Seen;
			const auto Locate = [&](auto&& Self, std::span<const FMaterialFunctionCallSnapshot> Calls) -> bool {
				if (Path.size() >= MaterialFunctionMaxCallDepth) return false;
				for (const auto& Call : Calls | std::views::take(MaterialProgramMaxNodeCount))
				{
					Path.push_back(Call.NodeId);
					if (Call.FunctionPath == Diagnostic.FunctionAssetPath) return true;
					if (Seen.emplace(Call.FunctionPath).second)
					{
						const auto Function = std::ranges::find(Input.Functions.Functions, Call.FunctionPath, &FMaterialFunctionSnapshot::AssetPath);
						if (Function != Input.Functions.Functions.end() && Self(Self, Function->Calls)) return true;
					}
					Path.pop_back();
				}
				return false;
			};
			if (Locate(Locate, Input.FunctionCalls)) Diagnostic.CallPath = std::move(Path);
		}
		if (Expander.Result) OutProgram = std::move(Expander.Program);
		return std::move(Expander.Result);
	}
}
