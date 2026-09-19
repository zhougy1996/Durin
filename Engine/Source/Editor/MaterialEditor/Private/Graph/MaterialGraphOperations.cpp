#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditSession.h"
#include "MaterialExpressionParameters.h"
#include "Asset/Asset.h"

#include "Graph/MaterialGraphValueTypes.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	auto FormatMaterialGraphCommandResult(const FMaterialGraphCommandResult& Result) -> std::string
	{
		if (Result) return {};
		std::string Message = Result.Message;
		if (!Result.Diagnostics.empty())
		{
			if (!Message.empty()) Message += " ";
			Message += FormatMaterialError(Result.Diagnostics.front().Error);
		}
		if (!Result.CleanupMessage.empty())
		{
			if (!Message.empty()) Message += " ";
			Message += "Cleanup: " + Result.CleanupMessage;
		}
		return Message.empty() ? "The material graph command failed." : Message;
	}

	namespace
	{
		auto GetSurfaceLink(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialSurfaceOutput Attribute) -> FMaterialExpressionInput*
		{
			if (Attribute > EMaterialSurfaceOutput::OpacityMask) return nullptr;
			return GetMaterialOutputInput(Outputs, static_cast<EMaterialOutputPin>(Attribute));
		}
		auto GetSurfaceDefault(const FMaterialExpressionSurfaceOutputs& Outputs, EMaterialSurfaceOutput Attribute) -> FMaterialProgramLiteral
		{
			const auto Value = ReadMaterialOutputDefault(Outputs, static_cast<EMaterialOutputPin>(Attribute));
			return Value.size() == 3 ? FMaterialProgramLiteral{Value[0], Value[1], Value[2]}
				: Value.size() == 1 ? FMaterialProgramLiteral{Value[0]} : FMaterialProgramLiteral{};
		}
	}

	namespace GraphEditInternals
	{
		auto ResolveParameterExpression(FGraphEditSession& State, DMaterialExpressionParameter& Parameter,
			const DMaterialExpressionParameter* Previous) -> FMaterialGraphCommandResult
		{
			auto Definition = Parameter.GetParameterDefinition();
			if (State.bFunction) return RejectCommand("Functions cannot own root parameters.");
			const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1));
			if (!Validation) return RejectCommand("The parameter definition is invalid. " + FormatMaterialError(FMaterialError(Validation)));
			const bool bEditingShared = Previous && Previous->Metadata.Name == Definition.Name
				&& Previous->Metadata.Id == Definition.Id;
			if (bEditingShared)
			{
				for (auto& Expression : State.Expressions)
					if (auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Id != Parameter.Id
						&& Peer->Metadata.Id == Definition.Id)
					{
						State.Modify(*Peer);
						if (const auto Applied = Peer->SetParameterDefinition(Definition); !Applied)
						{
							auto Result = RejectCommand("Shared parameter types must match; use a new parameter name to change type.");
							Result.Message += " " + FormatMaterialError(Applied.Error);
							return Result;
						}
					}
				return {};
			}
			for (const auto& Expression : State.Expressions)
				if (const auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Id != Parameter.Id
					&& Peer->Metadata.Name == Definition.Name)
				{
					if (const auto Applied = Parameter.SetParameterDefinition(Peer->GetParameterDefinition()); !Applied)
					{
						auto Result = RejectCommand("A parameter with this name already exists with a different type.");
						Result.Message += " " + FormatMaterialError(Applied.Error);
						return Result;
					}
					return {};
				}
			if (Previous && Previous->Metadata.Name != Definition.Name)
			{
				Parameter.Metadata.Id = FGuid::NewGuid();
				if (Parameter.Metadata.DisplayName == Previous->Metadata.Name.ToString())
					Parameter.Metadata.DisplayName = Parameter.Metadata.Name.ToString();
			}
			return {};
		}

		auto MakeParameterExpression(const FMaterialParameterDefinition& InputDefinition) -> FParameterExpressionResult
		{
			auto Definition = InputDefinition;
			if (const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1)); !Validation)
				return {.Message = "The parameter definition is invalid. " + FormatMaterialError(FMaterialError(Validation))};
			if (Definition.Type == EMaterialParameterType::Vector2 || Definition.Type == EMaterialParameterType::Vector)
			{
				Definition.Value = MakeParameterValue(EMaterialProgramValueType::Float4, ReadParameterLiteral(GetProgramType(Definition.Type), Definition.Value));
				Definition.Type = EMaterialParameterType::Vector4;
			}
			if (const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1)); !Validation)
				return {.Message = "The normalized parameter definition is invalid. " + FormatMaterialError(FMaterialError(Validation))};
			TStrongObjectPtr<DMaterialExpressionParameter> Result;
			switch (Definition.Type)
			{
			case EMaterialParameterType::Scalar:
			{
				auto* E = NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None);
				E->DefaultValue = Definition.Value.GetScalar();
				E->bHasRange = Definition.bHasRange; E->MinimumValue = Definition.MinimumValue; E->MaximumValue = Definition.MaximumValue;
				Result = E; break;
			}
			case EMaterialParameterType::Vector4:
			{
				auto* E = NewObject<DMaterialExpressionVector4Parameter>(nullptr, NAME_None);
				E->DefaultValue = Definition.Value.GetVector4(); Result = E; break;
			}
			case EMaterialParameterType::Texture:
			{
				auto* E = NewObject<DMaterialExpressionTextureParameter>(nullptr, NAME_None);
				const auto& Value = Definition.Value.GetTexture();
				E->DefaultValue = {Value.Texture, Value.SamplerState, Value.TextureFallback};
				E->TextureUsage = Definition.TextureUsage; Result = E; break;
			}
			default: return {.Message = "The parameter expression type is unsupported."};
			}
			Result->Id = FGuid::NewGuid();
			Result->Metadata = {Definition.Id, Definition.Name, Definition.DisplayName, Definition.GroupName,
				Definition.SortOrder, Definition.Presentation};
			return {.Expression = std::move(Result)};
		}
	}

	auto FMaterialGraphOperations::CreateParameter(
		DMaterial& Material, FMaterialParameterDefinition Definition, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		if (!Definition.Id.IsValid()) Definition.Id = FGuid::NewGuid();
		FGraphEditSession State(Material);
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return RejectCommand(std::move(Created.Message));
		auto Parameter = std::move(Created.Expression);
		const auto NodeId = Parameter->Id;
		if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return Error;
		Definition = Parameter->GetParameterDefinition();
		State.Presentation.Nodes.push_back({NodeId});
		State.Expressions.emplace_back(Parameter.Get());
		auto Result = State.Commit("Create Parameter", Transactions);
		if (Result)
		{
			Result.AffectedParameterIds = {Definition.Id};
			Result.GeneratedNodeIds = Result.AffectedNodeIds = {NodeId};
		}
		return Result;
	}

	auto FMaterialGraphOperations::RenameParameter(
		DMaterial& Material, const FGuid& ParameterId, FName Name, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Material);
		DMaterialExpressionParameter* Parameter = nullptr;
		for (const auto& Expression : State.Expressions)
			if (auto* E = Cast<DMaterialExpressionParameter>(Expression.Get()); E && E->Metadata.Id == ParameterId) { Parameter = E; break; }
		if (!Parameter) return RejectCommand("Parameter owner is unavailable.");
		if (Parameter->Metadata.Name == Name && Parameter->Metadata.DisplayName == Name.ToString())
			return {.Status = EMaterialGraphCommandStatus::NoChange, .AffectedParameterIds = {ParameterId}};
		for (const auto& Expression : State.Expressions)
			if (auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Metadata.Id == ParameterId)
			{
				State.Modify(*Peer);
				Peer->Metadata.Name = Name;
				Peer->Metadata.DisplayName = Name.ToString();
			}
		auto Result = State.Commit("Rename Parameter", Transactions);
		if (Result) Result.AffectedParameterIds = {ParameterId};
		return Result;
	}

	auto FMaterialGraphOperations::DeleteParameter(
		DMaterial& Material, const FGuid& ParameterId, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		std::vector<FGuid> Nodes;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == ParameterId)
				Nodes.push_back(Parameter->Id);
		if (Nodes.empty()) return RejectCommand("Parameter owner is unavailable.");
		return RemoveNodes(Material, Nodes, Transactions);
	}

	auto FMaterialGraphOperations::PromoteConstantToParameter(
		DMaterial& Material, const FGuid& NodeId, FName Name, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Material);
		const auto It = std::ranges::find(State.Expressions, NodeId, [](const auto& E) { return E->Id; });
		if (It == State.Expressions.end()) return RejectCommand("Only a numeric constant can be promoted to a parameter.");
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = Name; Definition.DisplayName = Name.ToString();
		if (const auto* E = Cast<DMaterialExpressionScalarConstant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeScalar(E->Value);
		else if (const auto* E = Cast<DMaterialExpressionVector2Constant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeVector2(E->Value);
		else if (const auto* E = Cast<DMaterialExpressionVector3Constant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeVector(E->Value);
		else if (const auto* E = Cast<DMaterialExpressionVector4Constant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeVector4(E->Value);
		else return RejectCommand("Only a numeric constant can be promoted to a parameter.");
		Definition.Type = Definition.Value.GetType();
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return RejectCommand(std::move(Created.Message));
		auto Parameter = std::move(Created.Expression);
		const auto Width = GetProgramType(Definition.Type);
		const bool bMask = Width == EMaterialProgramValueType::Float2 || Width == EMaterialProgramValueType::Float3;
		if (!bMask) Parameter->Id = NodeId;
		if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return Error;
		Definition = Parameter->GetParameterDefinition();
		if (bMask)
		{
			auto Mask = MakeParameterMask(*Parameter.Get(), Width, NodeId);
			*It = Mask.Get(); State.Expressions.emplace_back(Parameter.Get());
			const auto OldPosition = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
			const int32 X = OldPosition == State.Presentation.Nodes.end() ? -260 : OldPosition->X - 260;
			const int32 Y = OldPosition == State.Presentation.Nodes.end() ? 0 : OldPosition->Y;
			State.Presentation.Nodes.push_back({Parameter->Id, X, Y});
		}
		else *It = Parameter.Get();
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position != State.Presentation.Nodes.end()) Position->DisplayName = Definition.DisplayName;
		else State.Presentation.Nodes.push_back({.NodeId = NodeId, .DisplayName = Definition.DisplayName});
		auto Result = State.Commit("Promote Constant Parameter", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = {NodeId};
			Result.AffectedParameterIds = {Definition.Id};
		}
		return Result;
	}

	auto FMaterialGraphOperations::RemoveNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return FMaterialGraphDocument(Material).RemoveNodes(NodeIds, Transactions);
	}

	auto FMaterialGraphOperations::Connect(DMaterial& Material,
		const FMaterialGraphConnectRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (!Request.SourceNodeId.IsValid()) return RejectCommand("The material graph source node does not exist.");
		auto Result = FMaterialGraphDocument(Material).ConnectInput(Request.DestinationNodeId, Request.DestinationInputIndex,
			{Request.SourceNodeId, Request.SourceOutputIndex}, Request.bReplaceExisting, Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded)
		{
			Result.AffectedNodeIds = {Request.SourceNodeId, Request.DestinationNodeId};
			std::ranges::sort(Result.AffectedNodeIds);
		}
		return Result;
	}

	auto FMaterialGraphOperations::DisconnectInput(DMaterial& Material, const FGuid& DestinationNodeId,
		uint32 DestinationInputIndex, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		auto Result = FMaterialGraphDocument(Material).ConnectInput(DestinationNodeId, DestinationInputIndex, {}, true, Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {DestinationNodeId};
		return Result;
	}

	auto FMaterialGraphOperations::AssignSurfaceOutput(DMaterial& Material,
		const FMaterialGraphSurfaceOutputRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (!Request.SourceNodeId.IsValid()) return RejectCommand("The material graph source node does not exist.");
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput(Request.Output,
			{Request.SourceNodeId, Request.SourceOutputIndex}, Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {Request.SourceNodeId};
		return Result;
	}

	auto FMaterialGraphOperations::AssignAggregateSurface(DMaterial& Material, const FGuid& SourceNodeId,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (!SourceNodeId.IsValid()) return RejectCommand("Aggregate Surface requires a Surface node.");
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput({}, {SourceNodeId}, Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {SourceNodeId};
		return Result;
	}

	auto FMaterialGraphOperations::DisconnectAggregateSurface(DMaterial& Material,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		const auto SourceId = Material.GetExpressionOutputs().Surface.ExpressionId;
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput({}, {}, Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {SourceId};
		return Result;
	}

	auto FMaterialGraphOperations::DisconnectSurfaceOutput(DMaterial& Material,
		EMaterialSurfaceOutput Output, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		auto Outputs = Material.GetExpressionOutputs();
		const auto* Link = GetSurfaceLink(Outputs, Output);
		if (!Link) return RejectCommand("The material surface output is invalid.");
		const auto SourceId = Link->ExpressionId;
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput(Output, {}, Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {SourceId};
		return Result;
	}

	auto FMaterialGraphOperations::SetSurfaceDefault(DMaterial& Material,
		const FMaterialGraphSurfaceDefaultRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Material);
		const auto Previous = State.GetOutputs();
		const auto& Value = Request.Value;
		const auto Pin = static_cast<EMaterialOutputPin>(Request.Output);
		const std::array Components{Value.X, Value.Y, Value.Z};
		const auto Width = ReadMaterialOutputDefault(Previous, Pin).size();
		if (!WriteMaterialOutputDefault(State.GetOutputs(), Pin, std::span(Components).first(Width)))
			return RejectCommand("The material surface output is invalid.");
		if (State.GetOutputs() == Previous) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return State.Commit("Edit Material Surface Default", Transactions);
	}

	auto FMaterialGraphOperations::ResetSurfaceDefault(DMaterial& Material,
		EMaterialSurfaceOutput Output, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return SetSurfaceDefault(Material, {.Output = Output, .Value = GetSurfaceDefault({}, Output)}, Transactions);
	}

	auto FMaterialGraphOperations::SetParameterValue(
		DMaterial& Material,
		const FGuid& ParameterId,
		FMaterialParameterValue Value,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphParameterEditSession Session;
		const auto Begun = Session.Begin(Material, ParameterId, Transactions);
		if (!Begun) return Begun;
		auto Applied = Session.Apply(std::move(Value));
		if (!Applied) return Applied;
		auto Committed = Session.Commit();
		if (!Committed)
		{
			// A direct command has no interactive owner to retain a rejected preview.
			// Preserve its admission failure even if restoring the preview also fails.
			const auto Cleanup = Session.Cancel();
			if (!Cleanup) Committed.CleanupMessage = FormatMaterialGraphCommandResult(Cleanup);
			return Committed;
		}
		Committed.AffectedNodeIds = std::move(Applied.AffectedNodeIds);
		return Committed;
	}

	auto FMaterialGraphOperations::PromoteSurfaceOutputToParameter(DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Material);
		auto* Link = GetSurfaceLink(State.GetOutputs(), Request.Output);
		if (!Link) return RejectCommand("The material surface output is invalid.");
		if (Link->ExpressionId.IsValid()) return RejectCommand("Only an unconnected material surface output can be promoted.");
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = "SurfaceParameter";
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Definition.Name); ++Suffix)
			Definition.Name = FName(std::format("SurfaceParameter{}", Suffix));
		Definition.DisplayName = Definition.Name.ToString();
		const auto Type = GetMaterialSurfaceOutputType(Request.Output);
		Definition.Type = *GetParameterType(Type);
		Definition.Value = MakeParameterValue(Type, GetSurfaceDefault(State.GetOutputs(), Request.Output));
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return RejectCommand(std::move(Created.Message));
		auto Parameter = std::move(Created.Expression);
		const auto Id = Parameter->Id;
		*Link = {Id};
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y});
		State.Expressions.emplace_back(Parameter.Get());
		if (Type == EMaterialProgramValueType::Float2 || Type == EMaterialProgramValueType::Float3)
		{
			auto Mask = MakeParameterMask(*Parameter.Get(), Type);
			*Link = {Mask->Id}; State.Presentation.Nodes.push_back({Mask->Id, Request.X + 260, Request.Y});
			State.Expressions.emplace_back(Mask.Get());
		}
		auto Result = State.Commit("Promote Surface Parameter", Transactions);
		if (Result)
		{
			Result.GeneratedNodeIds = Result.AffectedNodeIds = {Id};
			Result.AffectedParameterIds = {Definition.Id};
		}
		return Result;
	}

	auto FMaterialGraphOperations::AddTextureToSurfaceOutput(DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Material);
		auto* Link = GetSurfaceLink(State.GetOutputs(), Request.Output);
		if (!Link) return RejectCommand("The material surface output is invalid.");
		TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Texture(NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None));
		Texture->Id = FGuid::NewGuid();
		Texture->Metadata.Id = FGuid::NewGuid(); Texture->Metadata.Name = "TextureParameter";
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Texture->Metadata.Name); ++Suffix)
			Texture->Metadata.Name = FName(std::format("TextureParameter{}", Suffix));
		Texture->Metadata.DisplayName = Texture->Metadata.Name.ToString();
		if (Request.Output == EMaterialSurfaceOutput::Normal)
		{
			Texture->TextureUsage = ETextureUsage::Normal;
			Texture->DefaultValue.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
		}
		const auto Id = Texture->Id, ParameterId = Texture->Metadata.Id;
		constexpr std::array<uint8, 8> Channels{1, 1, 4, 3, 2, 1, 5, 2};
		*Link = {Id, Channels[static_cast<size_t>(Request.Output)]};
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y, Texture->Metadata.DisplayName});
		State.Expressions.emplace_back(Texture.Get());
		auto Result = State.Commit("Add Material Surface Texture", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
			Result.AffectedParameterIds = {ParameterId};
		}
		return Result;
	}
}
