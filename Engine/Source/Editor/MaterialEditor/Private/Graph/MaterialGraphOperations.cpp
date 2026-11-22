#include "MaterialGraphEditInternals.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Materials/Material.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialGraphEditSession.h"
#include "MaterialExpressionParameters.h"
#include "Asset/Asset.h"

#include "Graph/MaterialGraphValueTypes.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	auto FormatMaterialGraphCommandResult(const FMaterialGraphCommandResult& Result) -> std::string
	{
		if (Result) return {};
		std::string Message = Result.Message;
		if (!Result.Diagnostics.empty())
		{
			if (!Message.empty()) Message += " ";
			Message += Result.Diagnostics.front().Message;
		}
		return Message.empty() ? "The material graph command failed." : Message;
	}

	auto MaterialEditing::CanPromoteSurfaceOutput(const DMaterial& Material, EMaterialProperty Property) -> bool
	{
		const auto Input = Material.GetNumericInputForProperty(Property);
		return Input && !Input->Connection.IsConnected() && Property != EMaterialProperty::MP_Normal;
	}

	auto MaterialEditing::SurfaceTextureOutput(const DMaterial& Material, EMaterialProperty Property) -> std::optional<EMaterialSampleOutput>
	{
		if (!Material.GetNumericInputForProperty(Property)) return {};
		using P = EMaterialProperty;
		switch (Property)
		{
		case P::MP_Normal: case P::MP_BaseColor: case P::MP_EmissiveColor: return EMaterialSampleOutput::RGB;
		case P::MP_Metallic: return EMaterialSampleOutput::B;
		case P::MP_Roughness: return EMaterialSampleOutput::G;
		case P::MP_Opacity: return EMaterialSampleOutput::A;
		case P::MP_AmbientOcclusion: case P::MP_OpacityMask: case P::MP_Specular:
		case P::MP_ClearCoat: case P::MP_ClearCoatRoughness: return EMaterialSampleOutput::R;
		default: return {}; // The compact sample uses implicit pixel derivatives.
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
			if (!Validation) return RejectCommand("The parameter definition is invalid. " + std::format("{} [parameter {}]", GetMaterialParameterErrorText(Validation.Error), Validation.ParameterId.ToString()));
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
							Result.Message += " " + Applied.error();
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
						Result.Message += " " + Applied.error();
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
				return {.Message = "The parameter definition is invalid. " + std::format("{} [parameter {}]", GetMaterialParameterErrorText(Validation.Error), Validation.ParameterId.ToString())};
			if (Definition.Type == EMaterialParameterType::Vector2 || Definition.Type == EMaterialParameterType::Vector)
			{
				Definition.Value = MakeParameterValue(EMaterialProgramValueType::Float4, ReadParameterLiteral(GetProgramType(Definition.Type), Definition.Value));
				Definition.Type = EMaterialParameterType::Vector4;
			}
			if (const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1)); !Validation)
				return {.Message = "The normalized parameter definition is invalid. " + std::format("{} [parameter {}]", GetMaterialParameterErrorText(Validation.Error), Validation.ParameterId.ToString())};
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
				E->DefaultValue = Value;
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

	auto MaterialEditing::CreateParameter(DMaterial& Material, FMaterialParameterDefinition Definition, ETransactionMode Mode)
		-> TMaterialGraphResult<DMaterialExpression*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		if (!Definition.Id.IsValid()) Definition.Id = FGuid::NewGuid();
		FGraphEditSession State(Material);
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return std::unexpected(RejectCommand(std::move(Created.Message)));
		auto Parameter = std::move(Created.Expression);
		const auto NodeId = Parameter->Id;
		if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return std::unexpected(Error);
		State.Presentation.Nodes.push_back({NodeId});
		State.Expressions.emplace_back(Parameter.Get());
		auto Result = State.Commit("Create Parameter", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return Parameter.Get();
	}

	auto MaterialEditing::RenameParameter(DMaterial& Material, const FGuid& ParameterId, FName Name, ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		FGraphEditSession State(Material);
		DMaterialExpressionParameter* Parameter = nullptr;
		for (const auto& Expression : State.Expressions)
			if (auto* E = Cast<DMaterialExpressionParameter>(Expression.Get()); E && E->Metadata.Id == ParameterId) { Parameter = E; break; }
		if (!Parameter) return RejectCommand("Parameter owner is unavailable.");
		if (Parameter->Metadata.Name == Name && Parameter->Metadata.DisplayName == Name.ToString())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		for (const auto& Expression : State.Expressions)
			if (auto* Peer = Cast<DMaterialExpressionParameter>(Expression.Get()); Peer && Peer->Metadata.Id == ParameterId)
			{
				State.Modify(*Peer);
				Peer->Metadata.Name = Name;
				Peer->Metadata.DisplayName = Name.ToString();
			}
		return State.Commit("Rename Parameter", Transactions);
	}

	auto MaterialEditing::DeleteParameter(DMaterial& Material, const FGuid& ParameterId, ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		std::vector<FGuid> Nodes;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == ParameterId)
				Nodes.push_back(Parameter->Id);
		if (Nodes.empty()) return RejectCommand("Parameter owner is unavailable.");
		return GraphEditInternals::RemoveGraphNodes(&Material, Nodes, Mode);
	}

	auto MaterialEditing::PromoteConstantToParameter(DMaterial& Material, const FGuid& NodeId, FName Name, ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
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
			State.ReplaceExpression(*It, *Mask);
			State.Expressions.emplace_back(Parameter.Get());
			const auto OldPosition = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
			const int32 X = OldPosition == State.Presentation.Nodes.end() ? -260 : OldPosition->X - 260;
			const int32 Y = OldPosition == State.Presentation.Nodes.end() ? 0 : OldPosition->Y;
			State.Presentation.Nodes.push_back({Parameter->Id, X, Y});
		}
		else State.ReplaceExpression(*It, *Parameter);
		const auto Position = std::ranges::find(State.Presentation.Nodes, NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position != State.Presentation.Nodes.end()) Position->DisplayName = Definition.DisplayName;
		else State.Presentation.Nodes.push_back({.NodeId = NodeId, .DisplayName = Definition.DisplayName});
		return State.Commit("Promote Constant Parameter", Transactions);
	}

	auto MaterialEditing::SetParameterValue(DMaterial& Material, const FGuid& ParameterId,
		FMaterialParameterValue Value,
		ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		FMaterialGraphParameterEditSession Session;
		const auto Begun = Session.Begin(Material, ParameterId, Transactions);
		if (!Begun) return RejectCommand(Begun.error());
		FScopedTransaction Recording("MaterialGraph", "Edit material parameter", nullptr, Transactions != nullptr);
		if (Transactions && !Recording.IsOutstanding()) return RejectCommand("The editor could not begin the material parameter transaction.");
		auto Applied = Session.Apply(std::move(Value));
		if (!Applied) return RejectCommand(Applied.error());
		std::expected<void, std::string> Committed;
		if (Transactions && !Transactions->IsActive()) Committed = std::unexpected("The parameter transaction was cancelled.");
		else Committed = Session.Commit();
		if (!Committed)
		{
			// A direct command has no interactive owner to retain a rejected preview.
			// Preserve its admission failure even if restoring the preview also fails.
			const auto Cleanup = Session.Cancel();
			if (!Cleanup) Committed.error() += " Failed to restore the parameter preview. " + Cleanup.error();
			return RejectCommand(std::move(Committed.error()));
		}
		return {.Status = *Applied ? EMaterialGraphCommandStatus::Succeeded : EMaterialGraphCommandStatus::NoChange};
	}

	auto MaterialEditing::PromoteSurfaceOutputToParameter(DMaterial& Material, const FMaterialGraphSurfaceNodeRequest& Request, ETransactionMode Mode)
		-> TMaterialGraphResult<DMaterialExpression*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		FGraphEditSession State(Material);
		if (!CanPromoteSurfaceOutput(Material, Request.Output))
			return std::unexpected(RejectCommand("This root input cannot be promoted to a numeric parameter."));
		auto* Link = State.EditRootInput(Request.Output);
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = "SurfaceParameter";
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Definition.Name); ++Suffix)
			Definition.Name = FName(std::format("SurfaceParameter{}", Suffix));
		Definition.DisplayName = Definition.Name.ToString();
		const auto Type = GetMaterialSurfaceOutputType(Request.Output);
		Definition.Type = *GetParameterType(Type);
		const auto Components = ReadMaterialNumericDefault(*Material.GetNumericInputForProperty(Request.Output), Request.Output);
		const FMaterialProgramLiteral Literal{Components[0], Components.size() > 1 ? Components[1] : 0.f,
			Components.size() > 2 ? Components[2] : 0.f};
		Definition.Value = MakeParameterValue(Type, Literal);
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return std::unexpected(RejectCommand(std::move(Created.Message)));
		auto Parameter = std::move(Created.Expression);
		const auto Id = Parameter->Id;
		*Link = {Parameter.Get()};
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y});
		State.Expressions.emplace_back(Parameter.Get());
		if (Type == EMaterialProgramValueType::Float2 || Type == EMaterialProgramValueType::Float3)
		{
			auto Mask = MakeParameterMask(*Parameter.Get(), Type);
			*Link = {Mask.Get()}; State.Presentation.Nodes.push_back({Mask->Id, Request.X + 260, Request.Y});
			State.Expressions.emplace_back(Mask.Get());
		}
		auto Result = State.Commit("Promote Surface Parameter", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return Parameter.Get();
	}

	auto MaterialEditing::AddTextureToSurfaceOutput(DMaterial& Material, const FMaterialGraphSurfaceNodeRequest& Request, ETransactionMode Mode)
		-> TMaterialGraphResult<DMaterialExpression*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		FGraphEditSession State(Material);
		const auto Channel = SurfaceTextureOutput(Material, Request.Output);
		if (!Channel) return std::unexpected(RejectCommand("This root input cannot use a compact texture sample."));
		auto* Link = State.EditRootInput(Request.Output);
		TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Texture(NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None));
		Texture->Id = FGuid::NewGuid();
		Texture->Metadata.Id = FGuid::NewGuid(); Texture->Metadata.Name = "TextureParameter";
		for (uint32 Suffix = 1; Material.FindParameterDefinition(Texture->Metadata.Name); ++Suffix)
			Texture->Metadata.Name = FName(std::format("TextureParameter{}", Suffix));
		Texture->Metadata.DisplayName = Texture->Metadata.Name.ToString();
		if (Request.Output == EMaterialProperty::MP_Normal)
		{
			Texture->TextureUsage = ETextureUsage::Normal;
			Texture->DefaultValue.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
		}
		const auto Id = Texture->Id;
		*Link = {Texture.Get(), static_cast<int32>(*Channel)};
		State.Presentation.Nodes.push_back({Id, Request.X, Request.Y, Texture->Metadata.DisplayName});
		State.Expressions.emplace_back(Texture.Get());
		auto Result = State.Commit("Add Material Surface Texture", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return Texture.Get();
	}
}
