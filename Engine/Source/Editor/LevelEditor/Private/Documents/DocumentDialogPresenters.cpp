#include "Documents/DocumentDialogPresenters.h"

#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	auto FLevelLoadingDialogPresenter::Draw(
		std::string_view LevelPath, const std::optional<FSceneLoadProgress>& Progress) -> bool
	{
		constexpr const char* Title = "Loading Level###Durin.LoadingLevel";
		if (Progress && !ImGui::IsPopupOpen(Title)) ImGui::OpenPopup(Title);
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		const float ContentWidth = MonaImGui::ScaleUI(360.0f);
		const float Padding = MonaImGui::ScaleUI(24.0f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(ContentWidth + Padding * 2.0f, 0.0f),
			ImVec2(ContentWidth + Padding * 2.0f, std::numeric_limits<float>::max()));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Padding, Padding));
		const bool bVisible = ImGui::BeginPopupModal(Title, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
		ImGui::PopStyleVar();
		if (!bVisible) return false;
		bool bCancel = false;
		if (!Progress)
			ImGui::CloseCurrentPopup();
		else
		{
			const float ContentLeft = ImGui::GetCursorPosX();
			const auto CenterText = [ContentLeft, ContentWidth](std::string_view Text, bool bMuted) {
				const float Width = ImGui::CalcTextSize(Text.data(), Text.data() + Text.size()).x;
				ImGui::SetCursorPosX(ContentLeft + std::max(0.0f, (ContentWidth - Width) * 0.5f));
				if (bMuted) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				const ImVec2 Position = ImGui::GetCursorScreenPos();
				ImGui::PushClipRect(Position, ImVec2(Position.x + ContentWidth, Position.y + ImGui::GetTextLineHeight()), true);
				ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
				ImGui::PopClipRect();
				if (bMuted) ImGui::PopStyleColor();
			};
			const auto Separator = LevelPath.find_last_of('/');
			const auto Name = LevelPath.substr(Separator == std::string_view::npos ? 0 : Separator + 1);
			const std::string Heading = std::format("Opening {}...", Name.substr(0, Name.find('.')));
			CenterText(Heading, true);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%.*s", static_cast<int>(LevelPath.size()), LevelPath.data());
			ImGui::Spacing();
			const auto Label = Progress->Phase == ESceneLoadPhase::Ready
				? std::string_view("Activating level") : GetSceneLoadPhaseLabel(Progress->Phase);
			CenterText(Label, false);
			ImGui::Spacing();
			ImGui::SetCursorPosX(ContentLeft);
			ImGui::ProgressBar(-1.0f - static_cast<float>(ImGui::GetTime()),
				ImVec2(ContentWidth, MonaImGui::ScaleUI(8.0f)), "");
			ImGui::Spacing();
			ImGui::SetCursorPosX(ContentLeft);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%.1f s elapsed", Progress->ElapsedSeconds);
			const float ButtonWidth = MonaImGui::ScaleUI(70.0f);
			ImGui::SameLine(ContentLeft + ContentWidth - ButtonWidth);
			bCancel = ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f));
		}
		ImGui::EndPopup();
		return bCancel;
	}

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
} // namespace Durin::Editor::Level
