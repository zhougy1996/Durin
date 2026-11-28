#pragma once

#include "CoreMinimal.h"
#include "Materials/Material.h"

#include "MaterialGraphOperations.h"
#include "Materials/MaterialFunction.h"
#include "DObject/ObjectLifecycle.h"
#include "Transactions/Transactor.h"

// Private helpers shared by material graph commands, layout, and edit sessions.
namespace Durin::MaterialEditor::GraphEditInternals
{
	auto RemoveGraphNodes(DObject* Owner, std::span<const FGuid> NodeIds, bool bShouldTransact) -> FMaterialGraphCommandResult;

	// MaterialEditor is the presentation boundary for command failures. Recovery
	// depends on status, never on message text or an implementation-specific cause.
	inline auto RejectCommand(std::string Message,
		std::vector<FMaterialProgramDiagnostic> Diagnostics = {},
		EMaterialGraphCommandStatus Status = EMaterialGraphCommandStatus::Rejected) -> FMaterialGraphCommandResult
	{
		return {.Status = Status, .Diagnostics = std::move(Diagnostics), .Message = std::move(Message)};
	}

	inline auto FailWithError(std::string* OutError, std::string_view Message) -> bool
	{
		if (OutError) *OutError = Message;
		return false;
	}

	// Admit the command before it inspects or prepares objects. Open an undo scope
	// only when RecordGraphEdit receives a validated change and its participants.
	template<class Command>
	auto WithGraphRecording(bool bShouldTransact, Command&& Edit)
	{
		using Result = std::invoke_result_t<Command, DTransactor*>;
		auto Admission = ResolveEditorTransactor(bShouldTransact);
		if (Admission) return Edit(*Admission);
		auto Failure = RejectCommand(std::move(Admission.error()));
		if constexpr (std::is_same_v<Result, FMaterialGraphCommandResult>) return Failure;
		else return Result(std::unexpected(std::move(Failure)));
	}

	template<class Command>
	auto WithGraphRecording(bool bShouldTransact, std::string* OutError, Command&& Edit) -> bool
	{
		auto Admission = ResolveEditorTransactor(bShouldTransact);
		if (!Admission) return FailWithError(OutError, Admission.error());
		return Edit(*Admission);
	}

	// Configuration publishes its own change and may reject the write.
	auto RecordGraphObjects(DObject& Owner, DTransactor* Transactions, std::string_view Description,
		std::span<DObject* const> Objects, bool bBatchGraphChanges,
		const std::function<FMaterialGraphCommandResult()>& Apply) -> FMaterialGraphCommandResult;

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

}
