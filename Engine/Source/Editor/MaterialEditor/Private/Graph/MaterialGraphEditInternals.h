#pragma once

#include "MaterialGraphOperations.h"
#include "Materials/MaterialFunction.h"
#include "DObject/ObjectLifecycle.h"
#include "Transactions/Transactor.h"

// Private helpers shared by material graph commands, layout, and edit sessions.
namespace Durin::Editor::Material::GraphEditInternals
{
	// MaterialEditor is the presentation boundary for command failures. Recovery
	// depends on status, never on message text or an implementation-specific cause.
	inline auto RejectCommand(std::string Message,
		std::vector<FMaterialProgramDiagnostic> Diagnostics = {},
		EMaterialGraphCommandStatus Status = EMaterialGraphCommandStatus::Rejected) -> FMaterialGraphCommandResult
	{
		return {.Status = Status, .Diagnostics = std::move(Diagnostics), .Message = std::move(Message)};
	}

	inline auto ReadGraphPresentation(const DObject& Owner) -> FMaterialGraphPresentation
	{
		if (const auto* Material = Cast<DMaterial>(&Owner)) return Material->GetMaterialGraphPresentation();
		return {.Nodes = Cast<DMaterialFunction>(&Owner)->GetFunctionPresentation().Nodes};
	}

	inline auto WriteGraphPresentation(DObject& Owner, FMaterialGraphPresentation Value)
		-> EMaterialGraphPresentationResult
	{
		if (auto* Material = Cast<DMaterial>(&Owner)) return Material->SetMaterialGraphPresentation(std::move(Value));
		auto* Function = Cast<DMaterialFunction>(&Owner);
		if (Value.Nodes == Function->GetFunctionPresentation().Nodes) return EMaterialGraphPresentationResult::NoChange;
		return Function->SetFunctionPresentation({.Nodes = std::move(Value.Nodes)})
			? EMaterialGraphPresentationResult::Changed : EMaterialGraphPresentationResult::Rejected;
	}

	MATERIALEDITOR_API auto MakeMaterialGraphPresentationTransaction(
		DObject& Material,
		const FMaterialGraphPresentation& BeforePresentation,
		const FMaterialGraphPresentation& AfterPresentation,
		std::string Description)
		-> std::unique_ptr<ITransactionCustomChange>;

	MATERIALEDITOR_API auto MakeMaterialGraphParameterTransaction(
		DMaterial& Material,
		FGuid ParameterId,
		FMaterialParameterValue BeforeValue,
		FMaterialParameterValue AfterValue)
		-> std::unique_ptr<ITransactionCustomChange>;

	auto CommitPresentationChange(
		DObject& Material,
		FMaterialGraphPresentation CandidatePresentation,
		std::string Description,
		std::vector<FGuid> Affected,
		DTransactor* Transactions) -> FMaterialGraphCommandResult;

}
