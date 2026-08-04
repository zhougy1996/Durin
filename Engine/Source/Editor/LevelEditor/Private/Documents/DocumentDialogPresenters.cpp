#include "Documents/DocumentDialogPresenters.h"

#include "MonaImGui.h"

namespace Durin
{
	auto FUnsavedLevelDialogPresenter::Draw(bool bRequestOpen, const FResolve& Resolve)
		-> std::optional<EUnsavedLevelDialogDecision>
	{
		if (bRequestOpen) ImGui::OpenPopup("Unsaved Level");

		if (!ImGui::BeginPopupModal(
			"Unsaved Level",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return std::nullopt;

		std::optional<EUnsavedLevelDialogDecision> Decision;
		ImGui::TextUnformatted("The current level has unsaved changes.");
		if (ImGui::Button("Save")) Decision = EUnsavedLevelDialogDecision::Save;
		ImGui::SameLine();
		if (ImGui::Button("Discard")) Decision = EUnsavedLevelDialogDecision::Discard;
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) Decision = EUnsavedLevelDialogDecision::Cancel;

		if (Decision && (!Resolve || Resolve(*Decision))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return Decision;
	}
} // namespace Durin
