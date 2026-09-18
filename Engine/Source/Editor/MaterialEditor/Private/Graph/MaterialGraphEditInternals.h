#pragma once

#include "MaterialGraphOperations.h"
#include "Materials/MaterialFunction.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transactor.h"

// Private helpers shared by material graph commands, layout, and edit sessions.
namespace Durin::Editor::Material::GraphEditInternals
{
	inline auto RejectDocument(FMaterialGraphDocumentError Error, std::vector<FMaterialProgramDiagnostic> Diagnostics = {}) -> FMaterialGraphCommandResult
	{
		FMaterialGraphCommandResult Result{.Diagnostics = std::move(Diagnostics)};
		Result.DocumentCause = std::move(Error);
		return Result;
	}

	inline auto MakeParameterRejected(FMaterialGraphParameterError Error) -> FMaterialGraphCommandResult
	{
		FMaterialGraphCommandResult Result;
		Result.ParameterCause = std::move(Error);
		return Result;
	}

	inline auto RejectSession(FMaterialGraphSessionError Error,
		std::vector<FMaterialProgramDiagnostic> Diagnostics = {}) -> FMaterialGraphCommandResult
	{
		FMaterialGraphCommandResult Result{.Diagnostics = std::move(Diagnostics)};
		Result.SessionCause = std::move(Error);
		return Result;
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

	auto MakeMaterialGraphPresentationTransaction(
		DObject& Material,
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
		DObject& Material,
		FMaterialGraphPresentation CandidatePresentation,
		std::string Description,
		std::vector<FGuid> Affected,
		DTransactor* Transactions) -> FMaterialGraphCommandResult;

}
