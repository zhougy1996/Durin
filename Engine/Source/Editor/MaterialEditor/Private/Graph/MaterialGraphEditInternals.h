#pragma once

#include "MaterialGraphOperations.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transactor.h"

// Private helpers shared by material graph commands, layout, and edit sessions.
namespace Durin::Editor::Material::GraphEditInternals
{
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

	auto CommitPresentationChange(
		DMaterial& Material,
		FMaterialGraphPresentation CandidatePresentation,
		std::string Description,
		std::vector<FGuid> Affected,
		DTransactor* Transactions) -> FMaterialGraphCommandResult;

}
