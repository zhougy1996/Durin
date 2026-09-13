#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "Asset/Asset.h"

#include "Graph/MaterialGraphValueTypes.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	auto FMaterialGraphOperations::CreateParameter(
		DMaterial& Material, FMaterialParameterDefinition Definition, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		FMaterialProgramNode Node;
		Node.Id = FGuid::NewGuid();
		if (!Definition.Id.IsValid()) Definition.Id = FGuid::NewGuid();
		Node.Opcode = Definition.Type == EMaterialParameterType::Texture
			? EMaterialProgramOpcode::TextureParameter : EMaterialProgramOpcode::Parameter;
		Node.ResultType = GetProgramType(Definition.Type);
		Node.Parameter = std::move(Definition);
		const auto Id = Node.Parameter.Id;
		auto Result = CreateNode(Material, {.Node = std::move(Node)}, Transactions);
		if (Result) Result.AffectedParameterIds = {Id};
		return Result;
	}

	auto FMaterialGraphOperations::RenameParameter(
		DMaterial& Material, const FGuid& ParameterId, FName Name, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		auto Program = *Material.GetMaterialProgram();
		auto Node = std::ranges::find_if(Program.Nodes,
			[&](const auto& Item) { return Item.Parameter.Id == ParameterId; });
		if (Node == Program.Nodes.end()) return MakeRejected("Parameter owner is unavailable.");
		Node->Parameter.Name = Name;
		Node->Parameter.DisplayName = Name.ToString();
		auto Result = ReplaceProgram(Material, std::move(Program), Transactions);
		if (Result) Result.AffectedParameterIds = {ParameterId};
		return Result;
	}

	auto FMaterialGraphOperations::DeleteParameter(
		DMaterial& Material, const FGuid& ParameterId, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		const auto& Nodes = Material.GetMaterialProgram()->Nodes;
		const auto Node = std::ranges::find_if(Nodes,
			[&](const auto& Item) { return Item.Parameter.Id == ParameterId; });
		if (Node == Nodes.end()) return MakeRejected("Parameter owner is unavailable.");
		const auto Id = Node->Id;
		return RemoveNodes(Material, std::span(&Id, 1), Transactions);
	}

	auto FMaterialGraphOperations::PromoteConstantToParameter(
		DMaterial& Material, const FGuid& NodeId, FName Name, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		auto Program = *Material.GetMaterialProgram();
		auto Node = std::ranges::find(Program.Nodes, NodeId, &FMaterialProgramNode::Id);
		if (Node == Program.Nodes.end() || Node->Opcode != EMaterialProgramOpcode::Constant)
			return MakeRejected("Only a numeric constant can be promoted to a parameter.");
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = Name;
		Definition.DisplayName = Name.ToString();
		const auto Type = GetParameterType(Node->ResultType);
		if (!Type || *Type == EMaterialParameterType::Texture)
			return MakeRejected("Only a numeric constant can be promoted to a parameter.");
		Definition.Type = *Type;
		Definition.Value = MakeParameterValue(Node->ResultType, Node->Literal);
		Node->Opcode = EMaterialProgramOpcode::Parameter;
		Node->Parameter = Definition;
		Node->DisplayName = Definition.DisplayName;
		Node->Literal = {};
		auto Result = ReplaceProgram(Material, std::move(Program), Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = {NodeId};
			Result.AffectedParameterIds = {Definition.Id};
		}
		return Result;
	}

	auto FMaterialGraphOperations::ReplaceProgram(DMaterial& Material,
		FMaterialProgram Program, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return ReplaceProgram(Material, std::move(Program), Material.GetMaterialGraphPresentation(), Transactions);
	}

	auto FMaterialGraphOperations::ReplaceProgram(DMaterial& Material,
		FMaterialProgram Program, FMaterialGraphPresentation Presentation,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphDocument Document(Material);
		FMaterialGraphDocumentState State;
		if (!Document.Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		State.Program = std::move(Program);
		State.Presentation = std::move(Presentation);
		return Document.Commit(std::move(State), "Edit Material Graph", Transactions);
	}

	auto FMaterialGraphOperations::CreateNode(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).CreateNode(std::move(Request), Transactions);
	}

	auto FMaterialGraphOperations::CreateNodeWithDefaultInputs(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		std::span<const std::vector<EMaterialProgramValueType>> AcceptedInputTypes,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (AcceptedInputTypes.size() != Request.Node.Inputs.size())
			return MakeRejected("The node palette input shape is stale.");
		if (!Request.Node.Parameter.Id.IsValid()
			&& (Request.Node.Opcode == EMaterialProgramOpcode::Parameter
				|| Request.Node.Opcode == EMaterialProgramOpcode::TextureParameter
				|| Request.Node.Opcode == EMaterialProgramOpcode::TextureSampleParameter2D))
		{
			FMaterialParameterDefinition Definition;
			Definition.Id = FGuid::NewGuid();
			const auto Type = GetParameterType(Request.Node.Opcode == EMaterialProgramOpcode::TextureSampleParameter2D
				? EMaterialProgramValueType::Texture2D : Request.Node.ResultType);
			if (!Type) return MakeRejected("Unsupported parameter type.");
			Definition.Type = *Type;
			const std::string BaseName = Definition.Type == EMaterialParameterType::Texture
				? "TextureParameter" : std::string(GetProgramTypeName(Request.Node.ResultType)) + "Parameter";
			Definition.Name = FName(BaseName);
			for (uint32 Suffix = 1; Material.FindParameterDefinition(Definition.Name); ++Suffix)
				Definition.Name = FName(std::format("{}{}", BaseName, Suffix));
			Definition.DisplayName = Definition.Name.ToString();
			if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
			const FGuid NodeId = Request.Node.Id;
			Request.Node.Parameter = Definition;
			Request.Node.DisplayName = Definition.DisplayName;
			auto Program = *Material.GetMaterialProgram();
			Program.Nodes.push_back(std::move(Request.Node));
			auto Presentation = Material.GetMaterialGraphPresentation();
			Presentation.Nodes.push_back({NodeId, Request.X, Request.Y});
			auto Result = ReplaceProgram(Material, std::move(Program), std::move(Presentation), Transactions);
			if (Result)
			{
				Result.GeneratedNodeIds = {NodeId};
				Result.AffectedNodeIds = {NodeId};
				Result.AffectedParameterIds = {Definition.Id};
			}
			return Result;
		}
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
		if (FindNode(Candidate, Request.Node.Id))
			return MakeRejected("The requested material graph node GUID already exists.");

		Request.Node.InputDefaults.resize(Request.Node.Inputs.size());
		for (size_t InputIndex = 0; InputIndex < Request.Node.Inputs.size(); ++InputIndex)
		{
			if (Request.Node.Inputs[InputIndex].SourceNodeId.IsValid()) continue;
			if (IsMaterialSampleUVInput(Request.Node, static_cast<uint32>(InputIndex))
				|| Request.Node.Opcode == EMaterialProgramOpcode::TextureCoordinates) continue;
			if (Request.Node.InputDefaults[InputIndex].Kind != EMaterialInputDefaultKind::None) continue;
			const auto NumericType = std::ranges::find_if(AcceptedInputTypes[InputIndex],
				[](EMaterialProgramValueType Type) {
					return Type < EMaterialProgramValueType::Texture2D;
				});
			if (NumericType == AcceptedInputTypes[InputIndex].end())
				return MakeRejected("This node requires a resource input that has no default value.");
			FMaterialProgramNode Default;
			Default.Id = FGuid::NewGuid();
			Default.Opcode = EMaterialProgramOpcode::Constant;
			Default.ResultType = *NumericType;
			float Value = 0.0f;
			if ((Request.Node.Opcode == EMaterialProgramOpcode::Multiply
				|| Request.Node.Opcode == EMaterialProgramOpcode::Divide)
				&& InputIndex == 1) Value = 1.0f;
			else if (Request.Node.Opcode == EMaterialProgramOpcode::Clamp
				&& InputIndex == 2) Value = 1.0f;
			else if (Request.Node.Opcode == EMaterialProgramOpcode::Lerp)
				Value = InputIndex == 1 ? 1.0f : InputIndex == 2 ? 0.5f : 0.0f;
			else if (Request.Node.Opcode == EMaterialProgramOpcode::Normalize)
				Value = 1.0f;
			Default.Literal = {Value, Value, Value, Value};
			if (Request.Node.Opcode == EMaterialProgramOpcode::MakeSurface)
				Default.Literal = GetMaterialSurfaceOutputDefault(FMaterialSurfaceOutputs{},
					static_cast<EMaterialSurfaceOutput>(InputIndex));
			Request.Node.InputDefaults[InputIndex] = {.Kind = EMaterialInputDefaultKind::Literal,
				.Type = Default.ResultType, .Literal = Default.Literal};
		}
		if (Candidate.Nodes.size() + 1 > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");

		const FGuid NodeId = Request.Node.Id;
		std::vector<FGuid> Generated{NodeId};
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		Candidate.Nodes.push_back(std::move(Request.Node));
		Presentation.Nodes.push_back({NodeId, Request.X, Request.Y});
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Create Material Node", Generated, Generated, Transactions);
	}

	auto FMaterialGraphOperations::ReplaceNode(
		DMaterial& Material,
		FMaterialProgramNode Node,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).ReplaceNode(std::move(Node), Transactions);
	}

	auto FMaterialGraphOperations::RemoveNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).RemoveNodes(NodeIds, Transactions);
	}

	auto FMaterialGraphOperations::Connect(
		DMaterial& Material,
		const FMaterialGraphConnectRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!FindNode(Candidate, Request.SourceNodeId))
			return MakeRejected("The material graph source node does not exist.");
		FMaterialProgramNode* Destination = FindNode(Candidate, Request.DestinationNodeId);
		if (!Destination)
			return MakeRejected("The material graph destination node does not exist.");
		if (Request.DestinationInputIndex >= Destination->Inputs.size())
			return MakeRejected("The material graph destination input does not exist.");
		const FMaterialProgramLink Link{Request.SourceNodeId, Request.SourceOutputIndex};
		if (Destination->Inputs[Request.DestinationInputIndex] == Link)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (!Request.bReplaceExisting)
			return MakeRejected("The material graph destination input is already connected.");
		Destination->Inputs[Request.DestinationInputIndex] = Link;
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Connect Material Nodes",
			{Request.SourceNodeId, Request.DestinationNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::DisconnectInput(
		DMaterial& Material,
		const FGuid& DestinationNodeId,
		uint32 DestinationInputIndex,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramNode* Destination = FindNode(Candidate, DestinationNodeId);
		if (!Destination)
			return MakeRejected("The material graph destination node does not exist.");
		if (DestinationInputIndex >= Destination->Inputs.size())
			return MakeRejected("The material graph destination input does not exist.");
		Destination->Inputs.erase(Destination->Inputs.begin() + DestinationInputIndex);
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Disconnect Material Input", {DestinationNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::AssignSurfaceOutput(
		DMaterial& Material,
		const FMaterialGraphSurfaceOutputRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!FindNode(Candidate, Request.SourceNodeId))
			return MakeRejected("The material graph source node does not exist.");
		FMaterialProgramLink* Output = &GetMaterialSurfaceOutputLink(
			Candidate.Outputs, Request.Output);
		const FMaterialProgramLink Link{Request.SourceNodeId, Request.SourceOutputIndex};
		if (*Output == Link) return {.Status = EMaterialGraphCommandStatus::NoChange};
		*Output = Link;
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Assign Material Surface Output", {Request.SourceNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::AssignAggregateSurface(
		DMaterial& Material, const FGuid& SourceNodeId,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		const FMaterialProgram* Current = Material.GetMaterialProgram();
		if (!Current) return MakeRejected("The material has no authored program.");
		const FMaterialProgramNode* Source = FindNode(*Current, SourceNodeId);
		if (!Source || Source->ResultType != EMaterialProgramValueType::Surface)
			return MakeRejected("Aggregate Material Output requires a Surface node.");
		FMaterialProgram Candidate = *Current;
		for (EMaterialSurfaceOutput Output : {
			EMaterialSurfaceOutput::BaseColor, EMaterialSurfaceOutput::Normal,
			EMaterialSurfaceOutput::Metallic, EMaterialSurfaceOutput::Roughness,
			EMaterialSurfaceOutput::AmbientOcclusion, EMaterialSurfaceOutput::Emissive,
			EMaterialSurfaceOutput::Opacity, EMaterialSurfaceOutput::OpacityMask})
			GetMaterialSurfaceOutputLink(Candidate.Outputs, Output) = {};
		Candidate.Outputs.Surface = {.SourceNodeId = SourceNodeId};
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Connect Aggregate Surface", {SourceNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::DisconnectAggregateSurface(
		DMaterial& Material, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		const FMaterialProgram* Current = Material.GetMaterialProgram();
		if (!Current) return MakeRejected("The material has no authored program.");
		if (!Current->Outputs.Surface.SourceNodeId.IsValid())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		FMaterialProgram Candidate = *Current;
		const FGuid Source = Candidate.Outputs.Surface.SourceNodeId;
		Candidate.Outputs.Surface = {};
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Disconnect Aggregate Surface", {Source}, {}, Transactions);
	}

	auto FMaterialGraphOperations::DisconnectSurfaceOutput(
		DMaterial& Material,
		EMaterialSurfaceOutput Output,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
			Candidate.Outputs, Output);
		if (!Link.SourceNodeId.IsValid())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		const FGuid Affected = Link.SourceNodeId;
		Link = {};
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Disconnect Material Surface Output", {Affected}, {}, Transactions);
	}

	auto FMaterialGraphOperations::SetSurfaceDefault(
		DMaterial& Material,
		const FMaterialGraphSurfaceDefaultRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramLiteral& Value = GetMaterialSurfaceOutputDefault(
			Candidate.Outputs, Request.Output);
		if (Value == Request.Value)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		Value = Request.Value;
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Edit Material Surface Default", {}, {}, Transactions);
	}

	auto FMaterialGraphOperations::ResetSurfaceDefault(
		DMaterial& Material,
		EMaterialSurfaceOutput Output,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return SetSurfaceDefault(Material, {
			.Output = Output,
			.Value = GetMaterialSurfaceOutputDefault(
				MakeDefaultMaterialProgram().Outputs, Output)}, Transactions);
	}

	auto FMaterialGraphOperations::SetParameterValue(
		DMaterial& Material,
		const FGuid& ParameterId,
		FMaterialParameterValue Value,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		FResolvedMaterialParameter Resolved;
		if (!Material.ResolveParameterValue(ParameterId, Resolved)
			|| !Resolved.Definition)
			return MakeRejected("The material parameter definition is unavailable.");
		const FMaterialParameterValue BeforeValue = Resolved.Value;
		if (BeforeValue == Value)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (!Material.SetParameterValue(ParameterId, Value))
			return MakeRejected("The material rejected the parameter value.");
		if (Transactions)
		{
			const auto bRecorded = Transactions->CommitApplied(
				MakeMaterialGraphParameterTransaction(
					Material, ParameterId, BeforeValue,
					std::move(Value)));
			check(bRecorded);
		}
		std::vector<FGuid> AffectedNodes;
		for (const FMaterialProgramNode& Node : Material.GetMaterialProgram()->Nodes)
			if (Node.Parameter.Id == ParameterId) AffectedNodes.push_back(Node.Id);
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(AffectedNodes)};
	}

	auto FMaterialGraphOperations::PromoteSurfaceOutputToParameter(
		DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (static_cast<uint8>(Request.Output)
			> static_cast<uint8>(EMaterialSurfaceOutput::OpacityMask))
			return MakeRejected("The material surface output is invalid.");
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		const FMaterialProgram BeforeProgram = *Material.GetMaterialProgram();
		if (GetMaterialSurfaceOutputLink(
			BeforeProgram.Outputs, Request.Output).SourceNodeId.IsValid())
			return MakeRejected(
				"Only an unconnected material surface output can be promoted.");
		if (BeforeProgram.Nodes.size() >= MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");
		auto Candidate = BeforeProgram;
		FMaterialProgramNode Node;
		Node.Id = FGuid::NewGuid();
		Node.Opcode = EMaterialProgramOpcode::Parameter;
		Node.ResultType = GetMaterialSurfaceOutputType(Request.Output);
		Node.Parameter.Id = FGuid::NewGuid();
		const std::string Name = "SurfaceParameter";
		Node.Parameter.Name = FName(Name);
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Node.Parameter.Name); ++Suffix)
			Node.Parameter.Name = FName(std::format("{}{}", Name, Suffix));
		Node.Parameter.DisplayName = Node.Parameter.Name.ToString();
		Node.Parameter.Type = *GetParameterType(Node.ResultType);
		Node.Parameter.Value = MakeParameterValue(Node.ResultType,
			GetMaterialSurfaceOutputDefault(Candidate.Outputs, Request.Output));
		const auto Id = Node.Id;
		const auto ParameterId = Node.Parameter.Id;
		Candidate.Nodes.push_back(std::move(Node));
		GetMaterialSurfaceOutputLink(Candidate.Outputs, Request.Output) = {Id};
		auto Presentation = Material.GetMaterialGraphPresentation();
		Presentation.Nodes.push_back({Id, Request.X, Request.Y});
		auto Result = ReplaceProgram(Material, std::move(Candidate), std::move(Presentation), Transactions);
		if (Result)
		{
			Result.GeneratedNodeIds = {Id};
			Result.AffectedNodeIds = {Id};
			Result.AffectedParameterIds = {ParameterId};
		}
		return Result;
	}

	auto FMaterialGraphOperations::AddTextureToSurfaceOutput(
		DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (static_cast<uint8>(Request.Output)
			> static_cast<uint8>(EMaterialSurfaceOutput::OpacityMask))
			return MakeRejected("The material surface output is invalid.");
		FMaterialGraphDocument Document(Material);
		FMaterialGraphDocumentState State;
		if (!Document.Capture(State)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto& Candidate = State.Program;
		const bool bNormal = Request.Output == EMaterialSurfaceOutput::Normal;
		const size_t RequiredNodeCount = bNormal ? 2u : 1u;
		if (Candidate.Nodes.size() + RequiredNodeCount
			> MaterialProgramMaxNodeCount)
			return MakeRejected(
				"Adding the texture branch would exceed the material graph node limit.");

		auto& Presentation = State.Presentation;
		std::vector<FGuid> Generated;
		auto AddNode = [&](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType Type,
			std::vector<FMaterialProgramLink> Inputs,
			int32 X, int32 Y) -> FMaterialProgramNode& {
			FMaterialProgramNode Node;
			Node.Id = FGuid::NewGuid();
			Node.Opcode = Opcode;
			Node.ResultType = Type;
			Node.Inputs = std::move(Inputs);
			const FGuid Id = Node.Id;
			Candidate.Nodes.push_back(std::move(Node));
			Presentation.Nodes.push_back({Id, X, Y});
			Generated.push_back(Id);
			return Candidate.Nodes.back();
		};
		FMaterialProgramNode& Texture = AddNode(
			bNormal ? EMaterialProgramOpcode::TextureParameter : EMaterialProgramOpcode::TextureSampleParameter2D,
			bNormal ? EMaterialProgramValueType::Texture2D : EMaterialProgramValueType::Float4,
			bNormal ? std::vector<FMaterialProgramLink>{} : std::vector<FMaterialProgramLink>{{}},
			bNormal ? Request.X - 320 : Request.X, Request.Y);
		Texture.Parameter.Id = FGuid::NewGuid();
		Texture.Parameter.Type = EMaterialParameterType::Texture;
		Texture.Parameter.Name = FName("TextureParameter");
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Texture.Parameter.Name); ++Suffix)
			Texture.Parameter.Name = FName(std::format("TextureParameter{}", Suffix));
		Texture.Parameter.DisplayName = Texture.Parameter.Name.ToString();
		if (bNormal)
		{
			Texture.Parameter.TextureUsage = ETextureUsage::Normal;
			Texture.Parameter.Value.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
		}
		Texture.DisplayName = Texture.Parameter.DisplayName;
		const FGuid ParameterId = Texture.Parameter.Id;
		constexpr std::array<uint8, 8> Channels{1, 6, 4, 3, 2, 1, 5, 2};
		FMaterialProgramLink Output{Texture.Id, Channels[static_cast<size_t>(Request.Output)]};
		if (bNormal)
		{
			FObjectPath Path;
			DMaterialFunction* Function = nullptr;
			if (!FObjectPath::TryCreate("/Engine/Materials/Functions/SampleNormal.SampleNormal", Path)
				|| !LoadObject(Path, Function) || !Function)
				return MakeRejected("The SampleNormal material function is unavailable.");
			const auto& Signature = Function->GetFunctionSignature();
			const auto Input = std::ranges::find(Signature.Inputs, "Texture", &FMaterialFunctionPort::Name);
			const auto Normal = std::ranges::find(Signature.Outputs, "Normal", &FMaterialFunctionPort::Name);
			if (Input == Signature.Inputs.end() || Input->Type != EMaterialProgramValueType::Texture2D
				|| Normal == Signature.Outputs.end() || Normal->Type != EMaterialProgramValueType::Float3)
				return MakeRejected("The SampleNormal material function has an incompatible interface.");
			const auto TextureId = Texture.Id;
			const auto& CallNode = AddNode(EMaterialProgramOpcode::FunctionCall,
				EMaterialProgramValueType::Float3, {}, Request.X, Request.Y);
			State.Calls.push_back({.NodeId = CallNode.Id, .Function = Function,
				.Inputs = {{Input->Id, EMaterialProgramValueType::Texture2D, {TextureId, 0}}},
				.Outputs = {{Normal->Id, Normal->Type}}});
			Output = {.SourceNodeId = CallNode.Id, .SourceOutputId = Normal->Id};
		}
		GetMaterialSurfaceOutputLink(Candidate.Outputs, Request.Output) = Output;
		auto Result = Document.Commit(std::move(State), "Add Material Surface Texture", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = Result.GeneratedNodeIds = std::move(Generated);
			Result.AffectedParameterIds = {ParameterId};
		}
		return Result;
	}
}
