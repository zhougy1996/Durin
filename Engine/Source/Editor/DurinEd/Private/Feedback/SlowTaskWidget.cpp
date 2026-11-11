#include "Feedback/SlowTaskWidget.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		auto DrawProgress(const FSlowTaskSnapshot& Task, bool bShowCancel, bool bShowElapsed, bool bModal) -> bool
		{
			const float ContentLeft = ImGui::GetCursorPosX();
			const float ContentWidth = MonaImGui::ScaleUI(360.0f);
			const auto Text = [&](std::string_view Value, bool bMuted) {
				ImGui::SetCursorPosX(ContentLeft);
				if (bMuted) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				if (bModal)
				{
					ImGui::PushTextWrapPos(ContentLeft + ContentWidth);
					ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
					ImGui::PopTextWrapPos();
				}
				else
				{
					const float Width = ImGui::CalcTextSize(Value.data(), Value.data() + Value.size()).x;
					ImGui::SetCursorPosX(ContentLeft + std::max(0.0f, (ContentWidth - Width) * 0.5f));
					const ImVec2 Position = ImGui::GetCursorScreenPos();
					ImGui::PushClipRect(Position, ImVec2(Position.x + ContentWidth, Position.y + ImGui::GetTextLineHeight()), true);
					ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
					ImGui::PopClipRect();
				}
				if (bMuted) ImGui::PopStyleColor();
				if (!Task.Details.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Task.Details.c_str());
			};
			const auto Gap = [&](float Height) {
				if (bModal) ImGui::Dummy(ImVec2(ContentWidth, MonaImGui::ScaleUI(Height)));
				else ImGui::Spacing();
			};
			const bool bHasTitle = !Task.Title.empty() && Task.Title != Task.Message;
			if (bHasTitle)
			{
				Text(Task.Title, !bModal);
				Gap(4.0f);
			}
			Text(Task.bCancelRequested && bModal ? "Canceling..." : Task.Message, bModal && bHasTitle);
			Gap(8.0f);
			ImGui::SetCursorPosX(ContentLeft);
			const float Fraction = Task.Fraction ? static_cast<float>(*Task.Fraction) : -1.0f - static_cast<float>(ImGui::GetTime());
			ImGui::ProgressBar(Fraction, ImVec2(ContentWidth, MonaImGui::ScaleUI(bModal ? 6.0f : 8.0f)), "");
			const bool bHasCancel = bShowCancel && Task.bCanCancel;
			if (!bModal || bShowElapsed || bHasCancel) Gap(10.0f);
			if (bShowElapsed)
			{
				ImGui::SetCursorPosX(ContentLeft);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("%.1f s elapsed", Task.ElapsedSeconds);
			}
			if (!bHasCancel) return false;
			const float ButtonWidth = MonaImGui::ScaleUI(bModal ? 96.0f : 70.0f);
			if (bShowElapsed) ImGui::SameLine(ContentLeft + ContentWidth - ButtonWidth);
			else ImGui::SetCursorPosX(ContentLeft + ContentWidth - ButtonWidth);
			ImGui::BeginDisabled(Task.bCancelRequested);
			const bool bCancel = ImGui::Button(Task.bCancelRequested ? "Canceling" : "Cancel", ImVec2(ButtonWidth, 0.0f));
			ImGui::EndDisabled();
			return bCancel;
		}
	}

	auto DrawSlowTaskProgress(const FSlowTaskSnapshot& Task, bool bShowCancel, bool bShowElapsed) -> bool
	{
		return DrawProgress(Task, bShowCancel, bShowElapsed, false);
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
		const float Width = MonaImGui::ScaleUI(360.0f) + Padding * 2.0f;
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
			else bCancel = DrawProgress(*Task, true, true, true);
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(3);
		ImGui::PopID();
		return bCancel;
	}
}
