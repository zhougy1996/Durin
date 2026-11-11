#include "Feedback/SlowTaskWidget.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		constexpr float ProgressWidth = 360.0f;

		auto ShowDetails(const FSlowTaskSnapshot& Task) -> void
		{
			if (!Task.Details.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Task.Details.c_str());
		}

		auto DrawProgressBar(const FSlowTaskSnapshot& Task, float Height) -> void
		{
			const float Fraction = Task.Fraction ? static_cast<float>(*Task.Fraction) : -1.0f - static_cast<float>(ImGui::GetTime());
			ImGui::ProgressBar(Fraction, ImVec2(MonaImGui::ScaleUI(ProgressWidth), MonaImGui::ScaleUI(Height)), "");
		}

		auto DrawProgressFooter(const FSlowTaskSnapshot& Task, float ContentLeft, bool bShowCancel, bool bShowElapsed, float ButtonWidth) -> bool
		{
			if (bShowElapsed)
			{
				ImGui::SetCursorPosX(ContentLeft);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("%.1f s elapsed", Task.ElapsedSeconds);
			}
			if (!bShowCancel || !Task.bCanCancel) return false;
			const float ButtonLeft = ContentLeft + MonaImGui::ScaleUI(ProgressWidth - ButtonWidth);
			if (bShowElapsed) ImGui::SameLine(ButtonLeft);
			else ImGui::SetCursorPosX(ButtonLeft);
			ImGui::BeginDisabled(Task.bCancelRequested);
			const bool bCancel = ImGui::Button(Task.bCancelRequested ? "Canceling" : "Cancel", ImVec2(MonaImGui::ScaleUI(ButtonWidth), 0.0f));
			ImGui::EndDisabled();
			return bCancel;
		}

		auto DrawModalProgress(const FSlowTaskSnapshot& Task) -> bool
		{
			const float ContentLeft = ImGui::GetCursorPosX();
			const float Width = MonaImGui::ScaleUI(ProgressWidth);
			ImGui::PushTextWrapPos(ContentLeft + Width);
			const bool bHasTitle = !Task.Title.empty() && Task.Title != Task.Message;
			if (bHasTitle)
			{
				ImGui::TextUnformatted(Task.Title.c_str());
				ShowDetails(Task);
				ImGui::Dummy(ImVec2(Width, MonaImGui::ScaleUI(4.0f)));
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			}
			ImGui::TextUnformatted(Task.bCancelRequested ? "Canceling..." : Task.Message.c_str());
			ShowDetails(Task);
			if (bHasTitle) ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(Width, MonaImGui::ScaleUI(8.0f)));
			DrawProgressBar(Task, 6.0f);
			ImGui::Dummy(ImVec2(Width, MonaImGui::ScaleUI(10.0f)));
			return DrawProgressFooter(Task, ContentLeft, true, true, 96.0f);
		}
	}

	auto DrawSlowTaskProgress(const FSlowTaskSnapshot& Task, bool bShowCancel, bool bShowElapsed) -> bool
	{
		const float ContentLeft = ImGui::GetCursorPosX();
		const float Width = MonaImGui::ScaleUI(ProgressWidth);
		const auto CenterText = [&](const std::string& Text) {
			ImGui::SetCursorPosX(ContentLeft + std::max(0.0f, (Width - ImGui::CalcTextSize(Text.c_str()).x) * 0.5f));
			const ImVec2 Position = ImGui::GetCursorScreenPos();
			ImGui::PushClipRect(Position, ImVec2(Position.x + Width, Position.y + ImGui::GetTextLineHeight()), true);
			ImGui::TextUnformatted(Text.c_str());
			ImGui::PopClipRect();
			ShowDetails(Task);
		};
		if (!Task.Title.empty() && Task.Title != Task.Message)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			CenterText(Task.Title);
			ImGui::PopStyleColor();
			ImGui::Spacing();
		}
		CenterText(Task.Message);
		ImGui::Spacing();
		ImGui::SetCursorPosX(ContentLeft);
		DrawProgressBar(Task, 8.0f);
		ImGui::Spacing();
		return DrawProgressFooter(Task, ContentLeft, bShowCancel, bShowElapsed, 70.0f);
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
		ImGui::SetNextWindowPos(ImVec2(Viewport->WorkPos.x + Viewport->WorkSize.x * 0.5f, Viewport->WorkPos.y + Viewport->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		const float Padding = MonaImGui::ScaleUI(28.0f);
		const float Width = MonaImGui::ScaleUI(ProgressWidth) + Padding * 2.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0), ImVec2(Width, std::numeric_limits<float>::max()));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Padding, Padding));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, MonaImGui::ScaleUI(8.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, MonaImGui::ScaleUI(3.0f));
		const bool bVisible = ImGui::BeginPopupModal(Title, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
		bool bCancel = false;
		if (bVisible)
		{
			if (!bShow) ImGui::CloseCurrentPopup();
			else bCancel = DrawModalProgress(*Task);
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(3);
		ImGui::PopID();
		return bCancel;
	}
}
