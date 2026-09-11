#include "MaterialGraphEditInternals.h"

#include "Graph/MaterialGraphValueTypes.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	auto FMaterialGraphOperations::CreateParameter(
		DMaterial& Material, FMaterialParameterDefinition Definition, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		return CommitDeclarationEdit(Material, Transactions,
			[&] { return Material.CreateParameterDefinition(std::move(Definition)); });
	}

	auto FMaterialGraphOperations::RenameParameter(
		DMaterial& Material, const FGuid& ParameterId, FName Name, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		return CommitDeclarationEdit(Material, Transactions,
			[&] { return Material.RenameParameterDefinition(ParameterId, Name); });
	}

	auto FMaterialGraphOperations::DeleteParameter(
		DMaterial& Material, const FGuid& ParameterId, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		return CommitDeclarationEdit(Material, Transactions,
			[&] { return Material.DeleteParameterDefinition(ParameterId); });
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
		std::vector<FMaterialParameterDefinition> Definitions(
			Material.GetParameterDefinitions().begin(), Material.GetParameterDefinitions().end());
		if (const auto* Existing = Material.FindParameterDefinition(Name))
		{
			if (Existing->Type != Definition.Type)
				return MakeRejected("The parameter name is already used by a different type.");
			Definition = *Existing;
		}
		else Definitions.push_back(Definition);
		Node->Opcode = EMaterialProgramOpcode::Parameter;
		Node->ParameterId = Definition.Id;
		Node->DisplayName = Definition.DisplayName;
		Node->Literal = {};
		auto Result = ReplaceDefinitionsAndProgram(Material, std::move(Definitions), std::move(Program), Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = {NodeId};
			Result.AffectedParameterIds = {Definition.Id};
		}
		return Result;
	}

	auto FMaterialGraphOperations::ReplaceDefinitionsAndProgram(
		DMaterial& Material,
		std::vector<FMaterialParameterDefinition> Definitions,
		FMaterialProgram Program,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return ReplaceDefinitionsAndProgram(Material, std::move(Definitions),
			std::move(Program), Material.GetMaterialGraphPresentation(), Transactions);
	}

	auto FMaterialGraphOperations::ReplaceDefinitionsAndProgram(
		DMaterial& Material,
		std::vector<FMaterialParameterDefinition> Definitions,
		FMaterialProgram Program,
		FMaterialGraphPresentation Presentation,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (!IsValid(&Material))
			return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		FMaterialDeclarationState Before{
			.Definitions = {Material.GetParameterDefinitions().begin(), Material.GetParameterDefinitions().end()},
			.Program = *Material.GetMaterialProgram(),
			.Presentation = Material.GetMaterialGraphPresentation()};
		Presentation = SanitizeMaterialGraphPresentation(Presentation, Program);
		if (Before.Definitions == Definitions && Before.Program == Program
			&& Before.Presentation == Presentation)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		auto Result = Material.SetMaterialDefinitionsAndProgram(Definitions, Program);
		if (!Result)
		{
			std::string Message(GetMaterialParameterErrorText(Result.Error));
			if (Result.ParameterId.IsValid())
				Message += std::format(" Parameter: {}", Result.ParameterId.ToString());
			return MakeRejected(std::move(Message), std::move(Result.Diagnostics));
		}
		Material.SetMaterialGraphPresentation(std::move(Presentation));
		if (Transactions)
		{
			FMaterialDeclarationState After{
				.Definitions = std::move(Definitions), .Program = *Material.GetMaterialProgram(),
				.Presentation = Material.GetMaterialGraphPresentation()};
			const auto bRecorded = Transactions->CommitApplied(
				MakeMaterialDeclarationTransaction(Material, std::move(Before), std::move(After)));
			check(bRecorded);
		}
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphOperations::CreateNode(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (Candidate.Nodes.size() >= MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");
		if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
		if (FindNode(Candidate, Request.Node.Id))
			return MakeRejected("The requested material graph node GUID already exists.");
		const FGuid GeneratedId = Request.Node.Id;
		Candidate.Nodes.push_back(std::move(Request.Node));
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		Presentation.Nodes.push_back({GeneratedId, Request.X, Request.Y});
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Create Material Node", {GeneratedId}, {GeneratedId}, Transactions);
	}

	auto FMaterialGraphOperations::CreateNodeWithDefaultInputs(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		std::span<const std::vector<EMaterialProgramValueType>> AcceptedInputTypes,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (AcceptedInputTypes.size() != Request.Node.Inputs.size())
			return MakeRejected("The node palette input shape is stale.");
		if (!Request.Node.ParameterId.IsValid()
			&& (Request.Node.Opcode == EMaterialProgramOpcode::Parameter
				|| Request.Node.Opcode == EMaterialProgramOpcode::TextureParameter))
		{
			FMaterialParameterDefinition Definition;
			Definition.Id = FGuid::NewGuid();
			const auto Type = GetParameterType(Request.Node.ResultType);
			if (!Type) return MakeRejected("Unsupported parameter type.");
			Definition.Type = *Type;
			const std::string BaseName = Definition.Type == EMaterialParameterType::Texture
				? "TextureParameter" : std::string(GetProgramTypeName(Request.Node.ResultType)) + "Parameter";
			Definition.Name = FName(BaseName);
			for (uint32 Suffix = 1; Material.FindParameterDefinition(Definition.Name); ++Suffix)
				Definition.Name = FName(std::format("{}{}", BaseName, Suffix));
			Definition.DisplayName = Definition.Name.ToString();
			std::vector<FMaterialParameterDefinition> Definitions(
				Material.GetParameterDefinitions().begin(), Material.GetParameterDefinitions().end());
			Definitions.push_back(Definition);
			if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
			const FGuid NodeId = Request.Node.Id;
			Request.Node.ParameterId = Definition.Id;
			Request.Node.DisplayName = Definition.DisplayName;
			auto Program = *Material.GetMaterialProgram();
			Program.Nodes.push_back(std::move(Request.Node));
			auto Presentation = Material.GetMaterialGraphPresentation();
			Presentation.Nodes.push_back({NodeId, Request.X, Request.Y});
			auto Result = ReplaceDefinitionsAndProgram(Material, std::move(Definitions),
				std::move(Program), std::move(Presentation), Transactions);
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

		std::vector<FMaterialProgramNode> Defaults;
		for (size_t InputIndex = 0; InputIndex < Request.Node.Inputs.size(); ++InputIndex)
		{
			if (Request.Node.Inputs[InputIndex].SourceNodeId.IsValid()) continue;
			const auto NumericType = std::ranges::find_if(AcceptedInputTypes[InputIndex],
				[](EMaterialProgramValueType Type) {
					return Type != EMaterialProgramValueType::Texture2D;
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
			Request.Node.Inputs[InputIndex] = {Default.Id, 0};
			Defaults.push_back(std::move(Default));
		}
		if (Candidate.Nodes.size() + Defaults.size() + 1 > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");

		const FGuid NodeId = Request.Node.Id;
		std::vector<FGuid> Generated{NodeId};
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		const FMaterialGraphCanvasMetrics& Metrics = FMaterialGraphGeometry::GetMetrics();
		for (size_t Index = 0; Index < Defaults.size(); ++Index)
		{
			Generated.push_back(Defaults[Index].Id);
			Presentation.Nodes.push_back({Defaults[Index].Id,
				Request.X - static_cast<int32>(Metrics.NodeWidth + Metrics.ColumnGap),
				Request.Y + static_cast<int32>(Index
					* (FMaterialGraphGeometry::GetNodeHeight(0) + Metrics.RowGap))});
			Candidate.Nodes.push_back(std::move(Defaults[Index]));
		}
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
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramNode* Existing = FindNode(Candidate, Node.Id);
		if (!Existing) return MakeRejected("The material graph node does not exist.");
		const FGuid AffectedId = Node.Id;
		*Existing = std::move(Node);
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Edit Material Node", {AffectedId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::RemoveNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (NodeIds.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph removal request exceeds the node bound.");
		std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		for (const FMaterialProgramNode& Node : Candidate.Nodes)
		{
			if (Removed.contains(Node.Id)) continue;
			if (std::ranges::any_of(Node.Inputs,
				[&](const FMaterialProgramLink& Link) {
					return Removed.contains(Link.SourceNodeId);
				}))
			{
				return MakeRejected(
					"A material graph node still depends on the requested selection. "
					"Reconnect or remove the dependent node first.");
			}
		}
		const size_t BeforeCount = Candidate.Nodes.size();
		std::erase_if(Candidate.Nodes, [&](const FMaterialProgramNode& Node) {
			return Removed.contains(Node.Id);
		});
		if (Candidate.Nodes.size() == BeforeCount)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		for (EMaterialSurfaceOutput Output : {
			EMaterialSurfaceOutput::BaseColor,
			EMaterialSurfaceOutput::Normal,
			EMaterialSurfaceOutput::Metallic,
			EMaterialSurfaceOutput::Roughness,
			EMaterialSurfaceOutput::AmbientOcclusion,
			EMaterialSurfaceOutput::Emissive,
			EMaterialSurfaceOutput::Opacity,
			EMaterialSurfaceOutput::OpacityMask})
		{
			FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
				Candidate.Outputs, Output);
			if (Removed.contains(Link.SourceNodeId)) Link = {};
		}
		if (Removed.contains(Candidate.Outputs.Surface.SourceNodeId))
			Candidate.Outputs.Surface = {};
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::erase_if(Presentation.Nodes, [&](const FMaterialGraphNodePresentation& Node) {
			return Removed.contains(Node.NodeId);
		});
		std::vector<FGuid> Affected(NodeIds.begin(), NodeIds.end());
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Delete Material Nodes", std::move(Affected), {}, Transactions);
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
		const std::vector Dependencies = InspectMaterialParameterDependencies(
			*Material.GetMaterialProgram(), Material.GetParameterDefinitions(), Material.GetMaterialFunctionCalls());
		if (std::ranges::none_of(Dependencies, [&](const auto& Dependency) {
			return Dependency.ParameterId == ParameterId;
		}))
			return MakeRejected(
				"Only a reachable material graph parameter can be edited here.");
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
			if (Node.ParameterId == ParameterId) AffectedNodes.push_back(Node.Id);
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
		const FGuid ParameterId = GetMaterialSurfaceParameterId(Request.Output,
			MaterialParameters::EMaterialBuiltinParameterKind::Value);
		FResolvedMaterialParameter BeforeResolved;
		if (!Material.ResolveParameterValue(ParameterId, BeforeResolved))
			return MakeRejected(
				"The material surface parameter definition is unavailable.");
		const EMaterialProgramValueType Type =
			GetMaterialSurfaceOutputType(Request.Output);
		const FMaterialParameterValue AfterValue = MakeParameterValue(
			Type, GetMaterialSurfaceOutputDefault(
				BeforeProgram.Outputs, Request.Output));

		FMaterialProgram Candidate = BeforeProgram;
		FMaterialProgramNode Node;
		Node.Id = FGuid::NewGuid();
		Node.Opcode = EMaterialProgramOpcode::Parameter;
		Node.ResultType = Type;
		Node.ParameterId = ParameterId;
		if (const FMaterialParameterDefinition* Definition =
			Material.FindParameterDefinition(ParameterId))
			Node.DisplayName = Definition->DisplayName;
		const FGuid NodeId = Node.Id;
		Candidate.Nodes.push_back(std::move(Node));
		GetMaterialSurfaceOutputLink(Candidate.Outputs, Request.Output) =
			{NodeId, 0};
		FMaterialGraphPresentation CandidatePresentation =
			Material.GetMaterialGraphPresentation();
		CandidatePresentation.Nodes.push_back(
			{NodeId, Request.X, Request.Y});
		const FMaterialGraphPresentation BeforePresentation =
			Material.GetMaterialGraphPresentation();
		FMaterialGraphCommandResult Result = CommitSemanticChange(Material, Candidate,
			CandidatePresentation, "Promote Material Surface Parameter",
			{NodeId}, {NodeId}, nullptr);
		if (!Result) return Result;
		if (!Material.SetParameterValue(ParameterId, AfterValue))
		{
			const auto RollbackValidation = Material.SetMaterialProgram(BeforeProgram);
			Material.SetMaterialGraphPresentation(BeforePresentation);
			return MakeRejected(
				"The promoted material parameter value could not be initialized.");
		}
		if (Transactions)
		{
			const auto bRecorded = Transactions->CommitApplied(
				MakeMaterialGraphSemanticTransaction(Material,
					BeforeProgram, BeforePresentation, Candidate,
					CandidatePresentation,
					"Promote Material Surface Parameter", ParameterId,
					BeforeResolved.Value, AfterValue));
			check(bRecorded);
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
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		const bool bNormal = Request.Output == EMaterialSurfaceOutput::Normal;
		const bool bVector = GetMaterialSurfaceOutputType(Request.Output)
			== EMaterialProgramValueType::Float3;
		const size_t RequiredNodeCount = bNormal ? 6u : 5u;
		if (Candidate.Nodes.size() + RequiredNodeCount
			> MaterialProgramMaxNodeCount)
			return MakeRejected(
				"Adding the texture branch would exceed the material graph node limit.");
		const FGuid TextureRole = GetMaterialSurfaceParameterId(Request.Output,
			MaterialParameters::EMaterialBuiltinParameterKind::Texture);
		FMaterialGraphPresentation Presentation =
			Material.GetMaterialGraphPresentation();
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
			EMaterialProgramOpcode::TextureParameter,
			EMaterialProgramValueType::Texture2D, {},
			Request.X - 640, Request.Y - 80);
		Texture.ParameterId = TextureRole;
		if (const FMaterialParameterDefinition* Definition =
			Material.FindParameterDefinition(TextureRole))
			Texture.DisplayName = Definition->DisplayName;
		const FGuid TextureId = Texture.Id;
		FMaterialProgramNode& Channel = AddNode(
			EMaterialProgramOpcode::Constant,
			EMaterialProgramValueType::Float, {},
			Request.X - 960, Request.Y + 80);
		Channel.Literal = {};
		FMaterialProgramNode& UV = AddNode(
			EMaterialProgramOpcode::UVChannel,
			EMaterialProgramValueType::Float2, {{Channel.Id, 0}},
			Request.X - 640, Request.Y + 80);
		const FGuid UVId = UV.Id;
		FMaterialProgramNode& Sample = AddNode(
			EMaterialProgramOpcode::TextureSample2D,
			EMaterialProgramValueType::Float4,
			{{TextureId, 0}, {UVId, 0}}, Request.X - 320, Request.Y);
		FGuid ResultId = Sample.Id;

		FMaterialProgramNode& Swizzle = AddNode(
			EMaterialProgramOpcode::Swizzle,
			bNormal ? EMaterialProgramValueType::Float2
				: GetMaterialSurfaceOutputType(Request.Output),
			{{Sample.Id, 0}}, Request.X, Request.Y);
		if (bNormal)
		{
			Swizzle.SwizzleLength = 2;
			Swizzle.SwizzleX = 0;
			Swizzle.SwizzleY = 1;
		}
		else if (bVector)
		{
			Swizzle.SwizzleLength = 3;
			Swizzle.SwizzleX = 0;
			Swizzle.SwizzleY = 1;
			Swizzle.SwizzleZ = 2;
		}
		else
		{
			constexpr std::array<uint8, 8> Components{0, 0, 2, 1, 0, 0, 3, 0};
			Swizzle.SwizzleLength = 1;
			Swizzle.SwizzleX = Components[static_cast<size_t>(Request.Output)];
		}
		ResultId = Swizzle.Id;
		if (bNormal)
		{
			FMaterialProgramNode& Decode = AddNode(
				EMaterialProgramOpcode::DecodeNormalRG,
				EMaterialProgramValueType::Float3,
				{{Swizzle.Id, 0}}, Request.X + 320, Request.Y);
			ResultId = Decode.Id;
		}
		GetMaterialSurfaceOutputLink(Candidate.Outputs, Request.Output) =
			{ResultId, 0};
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Add Material Surface Texture", Generated, Generated,
			Transactions);
	}
}
