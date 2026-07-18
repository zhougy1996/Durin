#include "Widgets/EditorRenameDialog.h"

#include "MonaImGui.h"

namespace Durin
{
	auto FEditorRenameDialog::Open(std::string_view InitialName) -> void
	{
		NameBuffer.fill(0);
		std::memcpy(NameBuffer.data(), InitialName.data(), std::min(InitialName.size(), NameBuffer.size() - 1));
		ValidationError.clear();
		bRequestOpen = true;
		bActive = true;
	}

	auto FEditorRenameDialog::Cancel() -> void
	{
		bRequestOpen = false;
		bActive = false;
		ValidationError.clear();
	}

	auto FEditorRenameDialog::Draw(const char* PopupTitle, std::string_view CurrentName, const FCommit& Commit) -> EEditorRenameDialogResult
	{
		if (bRequestOpen)
		{
			ImGui::OpenPopup(PopupTitle);
			bRequestOpen = false;
		}

		EEditorRenameDialogResult Result = EEditorRenameDialogResult::None;
		ImGui::SetNextWindowPos(ImGui::GetWindowViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(PopupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return Result;
		if (!bActive)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return EEditorRenameDialogResult::Cancelled;
		}

		ImGui::TextDisabled("Current name: %.*s", static_cast<int>(CurrentName.size()), CurrentName.data());
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(320.0f));
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		const bool bSubmitted = ImGui::InputText("Name", NameBuffer.data(), NameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		if (!ValidationError.empty())
			ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error), "%s", ValidationError.c_str());

		const bool bRenameClicked = ImGui::Button("Rename");
		ImGui::SameLine();
		const bool bCancelClicked = ImGui::Button("Cancel");
		if (bSubmitted || bRenameClicked)
		{
			const std::string_view NewName(NameBuffer.data());
			ValidationError = NewName.empty() ? "Name cannot be empty." : Commit(NewName);
			if (ValidationError.empty())
			{
				bActive = false;
				Result = EEditorRenameDialogResult::Renamed;
				ImGui::CloseCurrentPopup();
			}
		}
		else if (bCancelClicked || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			bActive = false;
			Result = EEditorRenameDialogResult::Cancelled;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return Result;
	}
} // namespace Durin
