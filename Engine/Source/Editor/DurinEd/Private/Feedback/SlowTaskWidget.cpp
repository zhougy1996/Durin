#include "Feedback/SlowTaskWidget.h"
#include "MonaImGui.h"

namespace Durin::Editor
{
	auto DrawSlowTaskProgress(const FSlowTaskSnapshot& Task, bool bShowCancel, bool bShowElapsed) -> bool
	{
		const float ContentLeft = ImGui::GetCursorPosX();
		const float ContentWidth = MonaImGui::ScaleUI(360.0f);
		const auto CenterText = [&Task, ContentLeft, ContentWidth](std::string_view Text, bool bMuted) {
			const float Width = ImGui::CalcTextSize(Text.data(), Text.data() + Text.size()).x;
			ImGui::SetCursorPosX(ContentLeft + std::max(0.0f, (ContentWidth - Width) * 0.5f));
			if (bMuted) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			const ImVec2 Position = ImGui::GetCursorScreenPos();
			ImGui::PushClipRect(Position, ImVec2(Position.x + ContentWidth, Position.y + ImGui::GetTextLineHeight()), true);
			ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
			ImGui::PopClipRect();
			if (bMuted) ImGui::PopStyleColor();
			if (!Task.Details.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Task.Details.c_str());
		};
		if (!Task.Title.empty() && Task.Title != Task.Message)
		{
			CenterText(Task.Title, true);
			ImGui::Spacing();
		}
		CenterText(Task.Message, false);
		ImGui::Spacing();
		ImGui::SetCursorPosX(ContentLeft);
		const float Fraction = Task.Fraction ? static_cast<float>(*Task.Fraction) : -1.0f - static_cast<float>(ImGui::GetTime());
		ImGui::ProgressBar(Fraction, ImVec2(ContentWidth, MonaImGui::ScaleUI(8.0f)), "");
		ImGui::Spacing();
		if (bShowElapsed)
		{
			ImGui::SetCursorPosX(ContentLeft);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%.1f s elapsed", Task.ElapsedSeconds);
		}
		if (!bShowCancel || !Task.bCanCancel) return false;
		const float ButtonWidth = MonaImGui::ScaleUI(70.0f);
		if (bShowElapsed) ImGui::SameLine(ContentLeft + ContentWidth - ButtonWidth);
		else ImGui::SetCursorPosX(ContentLeft + ContentWidth - ButtonWidth);
		ImGui::BeginDisabled(Task.bCancelRequested);
		const bool bCancel = ImGui::Button(Task.bCancelRequested ? "Canceling" : "Cancel", ImVec2(ButtonWidth, 0.0f));
		ImGui::EndDisabled();
		return bCancel;
	}

	auto FSlowTaskDialogPresenter::Draw(FFeedbackContext& Context) -> bool
	{
		const auto Task = Context.GetVisibleTask();
		return Draw(Task) && Context.RequestCancel(Task->Id);
	}

	auto FSlowTaskDialogPresenter::Draw(const std::optional<FSlowTaskSnapshot>& Task) -> bool
	{
		const bool bShow = Task && Task->Status == ESlowTaskStatus::Running;
		ImGui::PushID(this);
		constexpr const char* Title = "Working###Durin.SlowTask";
		if (bShow && !ImGui::IsPopupOpen(Title)) ImGui::OpenPopup(Title);
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		const float Padding = MonaImGui::ScaleUI(24.0f);
		const float Width = MonaImGui::ScaleUI(360.0f) + Padding * 2.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0), ImVec2(Width, std::numeric_limits<float>::max()));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Padding, Padding));
		const bool bVisible = ImGui::BeginPopupModal(Title, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
		ImGui::PopStyleVar();
		bool bCancel = false;
		if (bVisible)
		{
			if (!bShow) ImGui::CloseCurrentPopup();
			else bCancel = DrawSlowTaskProgress(*Task, true);
			ImGui::EndPopup();
		}
		ImGui::PopID();
		return bCancel;
	}
}
