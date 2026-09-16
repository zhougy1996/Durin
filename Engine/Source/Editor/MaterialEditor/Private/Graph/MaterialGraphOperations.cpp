#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphExpressionState.h"
#include "MaterialExpressionParameters.h"
#include "Asset/Asset.h"

#include "Graph/MaterialGraphValueTypes.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	namespace
	{
		auto GetSurfaceLink(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialSurfaceOutput Attribute) -> FMaterialExpressionInput*
		{
			const std::array Links{&Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic, &Outputs.Roughness,
				&Outputs.AmbientOcclusion, &Outputs.Emissive, &Outputs.Opacity, &Outputs.OpacityMask};
			const auto Index = static_cast<size_t>(Attribute);
			return Index < Links.size() ? Links[Index] : nullptr;
		}
		auto GetSurfaceDefault(const FMaterialExpressionSurfaceOutputs& Outputs, EMaterialSurfaceOutput Attribute) -> FMaterialProgramLiteral
		{
			const auto Vector = [](FVector3 Value) -> FMaterialProgramLiteral {
				return {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
			};
			switch (Attribute)
			{
			case EMaterialSurfaceOutput::BaseColor: return Vector(Outputs.BaseColorDefault);
			case EMaterialSurfaceOutput::Normal: return Vector(Outputs.NormalDefault);
			case EMaterialSurfaceOutput::Metallic: return {Outputs.MetallicDefault};
			case EMaterialSurfaceOutput::Roughness: return {Outputs.RoughnessDefault};
			case EMaterialSurfaceOutput::AmbientOcclusion: return {Outputs.AmbientOcclusionDefault};
			case EMaterialSurfaceOutput::Emissive: return Vector(Outputs.EmissiveDefault);
			case EMaterialSurfaceOutput::Opacity: return {Outputs.OpacityDefault};
			case EMaterialSurfaceOutput::OpacityMask: return {Outputs.OpacityMaskDefault};
			default: return {};
			}
		}
	}
	namespace GraphEditInternals
	{
		auto ResolveParameterExpression(FMaterialGraphDocumentState& State, DMaterialExpressionParameter& Parameter,
			const DMaterialExpressionParameter* Previous) -> std::string
		{
			if (State.bFunction) return "Functions cannot own root parameters.";
			auto Definition = Parameter.GetParameterDefinition();
			if (!ValidateMaterialParameterDefinitions(std::span(&Definition, 1))) return "The parameter definition is invalid.";
			const bool bEditingShared = Previous && Previous->Metadata.Name == Definition.Name
				&& Previous->Metadata.Id == Definition.Id;
			if (bEditingShared)
			{
				for (auto& Expression : State.Expressions)
					if (auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Id != Parameter.Id
						&& Peer->Metadata.Id == Definition.Id)
					{
						if (!Peer->SetParameterDefinition(Definition)) return "Shared parameter types must match; use a new parameter name to change type.";
					}
				return {};
			}
			for (const auto& Expression : State.Expressions)
				if (const auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Id != Parameter.Id
					&& Peer->Metadata.Name == Definition.Name)
				{
					if (!Parameter.SetParameterDefinition(Peer->GetParameterDefinition()))
						return "A parameter with this name already exists with a different type.";
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

		auto MakeParameterExpression(const FMaterialParameterDefinition& InputDefinition) -> TStrongObjectPtr<DMaterialExpressionParameter>
		{
			auto Definition = InputDefinition;
			if (!ValidateMaterialParameterDefinitions(std::span(&Definition, 1))) return {};
			if (Definition.Type == EMaterialParameterType::Vector2 || Definition.Type == EMaterialParameterType::Vector)
			{
				Definition.Value = MakeParameterValue(EMaterialProgramValueType::Float4, ReadParameterLiteral(GetProgramType(Definition.Type), Definition.Value));
				Definition.Type = EMaterialParameterType::Vector4;
			}
			if (!ValidateMaterialParameterDefinitions(std::span(&Definition, 1))) return {};
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
			default: return {};
			}
			Result->Id = FGuid::NewGuid();
			Result->Metadata = {Definition.Id, Definition.Name, Definition.DisplayName, Definition.GroupName,
				Definition.SortOrder, Definition.Presentation};
			return Result;
		}
	}

	auto FMaterialGraphOperations::CreateParameter(
		DMaterial& Material, FMaterialParameterDefinition Definition, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		if (!Definition.Id.IsValid()) Definition.Id = FGuid::NewGuid();
		const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1));
		if (!Validation) return MakeRejected(std::string(GetMaterialParameterErrorText(Validation.Error)));
		FOwnedGraphSnapshot State;
		if (!State.Capture(Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto Parameter = MakeParameterExpression(Definition);
		const auto NodeId = Parameter->Id;
		if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error.empty()) return MakeRejected(Error);
		Definition = Parameter->GetParameterDefinition();
		State.Presentation.Nodes.push_back({NodeId});
		State.Expressions.emplace_back(Parameter.Get());
		auto Result = CommitOwnedExpressions(Material, std::move(State), "Create Parameter", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!State.Capture(Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		DMaterialExpressionParameter* Parameter = nullptr;
		for (const auto& Expression : State.Expressions)
			if (auto* E = Cast<DMaterialExpressionParameter>(Expression.Get()); E && E->Metadata.Id == ParameterId) { Parameter = E; break; }
		if (!Parameter) return MakeRejected("Parameter owner is unavailable.");
		if (Parameter->Metadata.Name == Name && Parameter->Metadata.DisplayName == Name.ToString())
			return {.Status = EMaterialGraphCommandStatus::NoChange, .AffectedParameterIds = {ParameterId}};
		for (const auto& Expression : State.Expressions)
			if (auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Metadata.Id == ParameterId)
			{
				Peer->Metadata.Name = Name;
				Peer->Metadata.DisplayName = Name.ToString();
			}
		auto Result = CommitOwnedExpressions(Material, std::move(State), "Rename Parameter", Transactions);
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
		if (Nodes.empty()) return MakeRejected("Parameter owner is unavailable.");
		return RemoveNodes(Material, Nodes, Transactions);
	}

	auto FMaterialGraphOperations::PromoteConstantToParameter(
		DMaterial& Material, const FGuid& NodeId, FName Name, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		FOwnedGraphSnapshot State;
		if (!State.Capture(Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto It = std::ranges::find(State.Expressions, NodeId, [](const auto& E) { return E->Id; });
		if (It == State.Expressions.end()) return MakeRejected("Only a numeric constant can be promoted to a parameter.");
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = Name; Definition.DisplayName = Name.ToString();
		if (const auto* E = Cast<DMaterialExpressionScalarConstant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeScalar(E->Value);
		else if (const auto* E = Cast<DMaterialExpressionVector2Constant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeVector2(E->Value);
		else if (const auto* E = Cast<DMaterialExpressionVector3Constant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeVector(E->Value);
		else if (const auto* E = Cast<DMaterialExpressionVector4Constant>(It->Get())) Definition.Value = FMaterialParameterValue::MakeVector4(E->Value);
		else return MakeRejected("Only a numeric constant can be promoted to a parameter.");
		Definition.Type = Definition.Value.GetType();
		const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1));
		if (!Validation) return MakeRejected(std::string(GetMaterialParameterErrorText(Validation.Error)));
		auto Parameter = MakeParameterExpression(Definition);
		const auto Width = GetProgramType(Definition.Type);
		const bool bMask = Width == EMaterialProgramValueType::Float2 || Width == EMaterialProgramValueType::Float3;
		if (!bMask) Parameter->Id = NodeId;
		if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error.empty()) return MakeRejected(Error);
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
		auto Result = CommitOwnedExpressions(Material, std::move(State), "Promote Constant Parameter", Transactions);
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
		if (!Request.SourceNodeId.IsValid()) return MakeRejected("The material graph source node does not exist.");
		auto Result = FMaterialGraphDocument(Material).ConnectInput(Request.DestinationNodeId, Request.DestinationInputIndex,
			{Request.SourceNodeId, Request.SourceOutputIndex}, Request.bReplaceExisting, Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded)
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
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {DestinationNodeId};
		return Result;
	}

	auto FMaterialGraphOperations::AssignSurfaceOutput(DMaterial& Material,
		const FMaterialGraphSurfaceOutputRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (!Request.SourceNodeId.IsValid()) return MakeRejected("The material graph source node does not exist.");
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput(Request.Output,
			{Request.SourceNodeId, Request.SourceOutputIndex}, Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {Request.SourceNodeId};
		return Result;
	}

	auto FMaterialGraphOperations::AssignAggregateSurface(DMaterial& Material, const FGuid& SourceNodeId,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (!SourceNodeId.IsValid()) return MakeRejected("Aggregate Surface requires a Surface node.");
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput({}, {SourceNodeId}, Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {SourceNodeId};
		return Result;
	}

	auto FMaterialGraphOperations::DisconnectAggregateSurface(DMaterial& Material,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		const auto SourceId = Material.GetExpressionOutputs().Surface.ExpressionId;
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput({}, {}, Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {SourceId};
		return Result;
	}

	auto FMaterialGraphOperations::DisconnectSurfaceOutput(DMaterial& Material,
		EMaterialSurfaceOutput Output, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		auto Outputs = Material.GetExpressionOutputs();
		const auto* Link = GetSurfaceLink(Outputs, Output);
		if (!Link) return MakeRejected("The material surface output is invalid.");
		const auto SourceId = Link->ExpressionId;
		auto Result = FMaterialGraphDocument(Material).AssignMaterialOutput(Output, {}, Transactions);
		if (Result.Status == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds = {SourceId};
		return Result;
	}

	auto FMaterialGraphOperations::SetSurfaceDefault(DMaterial& Material,
		const FMaterialGraphSurfaceDefaultRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FOwnedGraphSnapshot State;
		if (!State.Capture(Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		const auto Previous = State.GetOutputs();
		const auto& Value = Request.Value;
		switch (Request.Output)
		{
		case EMaterialSurfaceOutput::BaseColor: State.GetOutputs().BaseColorDefault = FVector3(Value.X, Value.Y, Value.Z); break;
		case EMaterialSurfaceOutput::Normal: State.GetOutputs().NormalDefault = FVector3(Value.X, Value.Y, Value.Z); break;
		case EMaterialSurfaceOutput::Metallic: State.GetOutputs().MetallicDefault = Value.X; break;
		case EMaterialSurfaceOutput::Roughness: State.GetOutputs().RoughnessDefault = Value.X; break;
		case EMaterialSurfaceOutput::AmbientOcclusion: State.GetOutputs().AmbientOcclusionDefault = Value.X; break;
		case EMaterialSurfaceOutput::Emissive: State.GetOutputs().EmissiveDefault = FVector3(Value.X, Value.Y, Value.Z); break;
		case EMaterialSurfaceOutput::Opacity: State.GetOutputs().OpacityDefault = Value.X; break;
		case EMaterialSurfaceOutput::OpacityMask: State.GetOutputs().OpacityMaskDefault = Value.X; break;
		default: return MakeRejected("The material surface output is invalid.");
		}
		if (State.GetOutputs() == Previous) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return CommitOwnedExpressions(Material, std::move(State), "Edit Material Surface Default", Transactions);
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
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == ParameterId)
				AffectedNodes.push_back(Parameter->Id);
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(AffectedNodes)};
	}

	auto FMaterialGraphOperations::PromoteSurfaceOutputToParameter(DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FOwnedGraphSnapshot State;
		if (!State.Capture(Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Link = GetSurfaceLink(State.GetOutputs(), Request.Output);
		if (!Link) return MakeRejected("The material surface output is invalid.");
		if (Link->ExpressionId.IsValid()) return MakeRejected("Only an unconnected material surface output can be promoted.");
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = "SurfaceParameter";
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Definition.Name); ++Suffix)
			Definition.Name = FName(std::format("SurfaceParameter{}", Suffix));
		Definition.DisplayName = Definition.Name.ToString();
		const auto Type = GetMaterialSurfaceOutputType(Request.Output);
		Definition.Type = *GetParameterType(Type);
		Definition.Value = MakeParameterValue(Type, GetSurfaceDefault(State.GetOutputs(), Request.Output));
		auto Parameter = MakeParameterExpression(Definition);
		const auto Id = Parameter->Id;
		*Link = {Id}; State.GetOutputs().Surface = {};
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y});
		State.Expressions.emplace_back(Parameter.Get());
		if (Type == EMaterialProgramValueType::Float2 || Type == EMaterialProgramValueType::Float3)
		{
			auto Mask = MakeParameterMask(*Parameter.Get(), Type);
			*Link = {Mask->Id}; State.Presentation.Nodes.push_back({Mask->Id, Request.X + 260, Request.Y});
			State.Expressions.emplace_back(Mask.Get());
		}
		auto Result = CommitOwnedExpressions(Material, std::move(State), "Promote Surface Parameter", Transactions);
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
		FOwnedGraphSnapshot State;
		if (!State.Capture(Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		auto* Link = GetSurfaceLink(State.GetOutputs(), Request.Output);
		if (!Link) return MakeRejected("The material surface output is invalid.");
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
		*Link = {Id, Channels[static_cast<size_t>(Request.Output)]}; State.GetOutputs().Surface = {};
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y, Texture->Metadata.DisplayName});
		State.Expressions.emplace_back(Texture.Get());
		auto Result = CommitOwnedExpressions(Material, std::move(State), "Add Material Surface Texture", Transactions);
		if (Result)
		{
			Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
			Result.AffectedParameterIds = {ParameterId};
		}
		return Result;
	}
}
