#pragma once

#include "MaterialGraphOperations.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transactor.h"

// Private helpers shared by material graph commands, layout, and edit sessions.
namespace Durin::Editor::Material::GraphEditInternals
{
	inline auto FindNode(FMaterialProgram& Program, const FGuid& Id)
		-> FMaterialProgramNode*
	{
		const auto It = std::ranges::find(Program.Nodes, Id,
			&FMaterialProgramNode::Id);
		return It == Program.Nodes.end() ? nullptr : &*It;
	}

	inline auto FindNode(const FMaterialProgram& Program, const FGuid& Id)
		-> const FMaterialProgramNode*
	{
		const auto It = std::ranges::find(Program.Nodes, Id,
			&FMaterialProgramNode::Id);
		return It == Program.Nodes.end() ? nullptr : &*It;
	}

	inline auto MakeRejected(
		std::string Message,
		std::vector<FMaterialProgramDiagnostic> Diagnostics = {})
		-> FMaterialGraphCommandResult
	{
		return {
			.Status = EMaterialGraphCommandStatus::Rejected,
			.Diagnostics = std::move(Diagnostics),
			.Message = std::move(Message),
		};
	}

	// Captures declarations and graph state as one undoable material edit.
	struct FMaterialDeclarationState
	{
		std::vector<FMaterialParameterDefinition> Definitions;
		FMaterialProgram Program;
		FMaterialGraphPresentation Presentation;
	};

	auto MakeMaterialDeclarationTransaction(DMaterial& Material,
		FMaterialDeclarationState Before, FMaterialDeclarationState After)
		-> std::unique_ptr<ITransactionCustomChange>;

	auto MakeMaterialGraphSemanticTransaction(
		DMaterial& Material,
		FMaterialProgram BeforeProgram,
		FMaterialGraphPresentation BeforePresentation,
		FMaterialProgram AfterProgram,
		FMaterialGraphPresentation AfterPresentation,
		std::string Description,
		FGuid ParameterId = {},
		FMaterialParameterValue BeforeParameterValue = {},
		FMaterialParameterValue AfterParameterValue = {})
		-> std::unique_ptr<ITransactionCustomChange>;

	auto MakeMaterialGraphPresentationTransaction(
		DMaterial& Material,
		const FMaterialGraphPresentation& BeforePresentation,
		const FMaterialGraphPresentation& AfterPresentation,
		std::string Description)
		-> std::unique_ptr<ITransactionCustomChange>;

	auto MakeMaterialGraphParameterTransaction(
		DMaterial& Material,
		FGuid ParameterId,
		FMaterialParameterValue BeforeValue,
		FMaterialParameterValue AfterValue)
		-> std::unique_ptr<ITransactionCustomChange>;

	auto CommitSemanticChange(
		DMaterial& Material,
		FMaterialProgram CandidateProgram,
		FMaterialGraphPresentation CandidatePresentation,
		std::string Description,
		std::vector<FGuid> Affected,
		std::vector<FGuid> Generated,
		DTransactor* Transactions)
		-> FMaterialGraphCommandResult;

	auto CommitPresentationChange(
		DMaterial& Material,
		FMaterialGraphPresentation CandidatePresentation,
		std::string Description,
		std::vector<FGuid> Affected,
		DTransactor* Transactions) -> FMaterialGraphCommandResult;

	template<typename TEdit>
	auto CommitDeclarationEdit(DMaterial& Material, DTransactor* Transactions, TEdit&& Edit)
		-> FMaterialGraphCommandResult
	{
		if (!IsValid(&Material)) return {.Status = EMaterialGraphCommandStatus::StaleOwner};
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		FMaterialDeclarationState Before{
			.Definitions = {Material.GetParameterDefinitions().begin(), Material.GetParameterDefinitions().end()},
			.Program = *Material.GetMaterialProgram(),
			.Presentation = Material.GetMaterialGraphPresentation()};
		auto Result = Edit();
		if (!Result)
			return MakeRejected(std::format("Parameter {}: {}", Result.ParameterId.ToString(),
				GetMaterialParameterErrorText(Result.Error)), std::move(Result.Diagnostics));
		FMaterialDeclarationState After{
			.Definitions = {Material.GetParameterDefinitions().begin(), Material.GetParameterDefinitions().end()},
			.Program = *Material.GetMaterialProgram(),
			.Presentation = Material.GetMaterialGraphPresentation()};
		const bool bChanged = Before.Definitions != After.Definitions || Before.Program != After.Program;
		if (bChanged && Transactions)
		{
			const auto bRecorded = Transactions->CommitApplied(MakeMaterialDeclarationTransaction(
				Material, std::move(Before), std::move(After)));
			check(bRecorded);
		}
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded : EMaterialGraphCommandStatus::NoChange,
			.AffectedParameterIds = {Result.ParameterId}};
	}

}
