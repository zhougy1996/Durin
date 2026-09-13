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

	auto MakeMaterialGraphSemanticTransaction(
		DMaterial& Material,
		FMaterialProgram BeforeProgram,
		FMaterialGraphPresentation BeforePresentation,
		FMaterialProgram AfterProgram,
		FMaterialGraphPresentation AfterPresentation,
		std::string Description)
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

}
