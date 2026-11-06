#include "Documents/EditorDocumentUI.h"
#include "Documents/EditorDocumentManager.h"
#include "MonaImGui.h"

namespace Durin::DocumentUI
{
	auto FindPendingClose(std::span<FEditorDocumentManager* const> Controllers) -> FEditorDocumentManager*
	{
		FEditorDocumentManager* Selected = nullptr;
		for (auto* Controller : Controllers)
		{
			if (!Controller || !Controller->GetPendingCloseDocument()) continue;
			if (!Selected || Controller->GetPendingCloseSequence() < Selected->GetPendingCloseSequence()) Selected = Controller;
		}
		return Selected;
	}

	auto DrawCloseConfirmation(FEditorDocumentManager& DocumentManager) -> void
	{
		const FEditorDocument* PendingDocument = DocumentManager.GetPendingCloseDocument();
		if (!PendingDocument) return;

		ImGui::OpenPopup("Confirm Close Document###Durin.Editor.DocumentCloseConfirmation");
		if (!ImGui::BeginPopupModal(
				"Confirm Close Document###Durin.Editor.DocumentCloseConfirmation",
				nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
			))
			return;

		ImGui::TextWrapped("Save changes to \"%s\" before closing?", PendingDocument->Label.c_str());
		ImGui::Spacing();
		if (ImGui::Button("Save", ImVec2(100, 0)))
		{
			if (DocumentManager.ResolvePendingDocumentClose(EDocumentCloseResponse::Save) ==
				EDocumentCloseResult::Closed)
				ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(100, 0)))
		{
			if (DocumentManager.ResolvePendingDocumentClose(EDocumentCloseResponse::Discard) ==
				EDocumentCloseResult::Closed)
				ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0)))
		{
			DocumentManager.ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
