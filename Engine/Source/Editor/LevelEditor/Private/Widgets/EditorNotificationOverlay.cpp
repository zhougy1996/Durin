#include "Widgets/EditorNotificationOverlay.h"

#include "Editor/EditorNotification.h"
#include "Editor/EditorTransaction.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Icons/FontAwesomeIcons.h"
#include "LevelEditorWorkspace.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		constexpr size_t MaxVisibleNotifications = 5;

		auto ThemeColor(EEditorNotificationType Type) -> MonaImGui::EUIThemeColor
		{
			switch (Type)
			{
			case EEditorNotificationType::Success: return MonaImGui::EUIThemeColor::Success;
			case EEditorNotificationType::Warning: return MonaImGui::EUIThemeColor::Warning;
			case EEditorNotificationType::Error: return MonaImGui::EUIThemeColor::Error;
			case EEditorNotificationType::Progress:
			case EEditorNotificationType::Info:
			default: return MonaImGui::EUIThemeColor::Info;
			}
		}

		auto TypeIcon(EEditorNotificationType Type) -> const char*
		{
			switch (Type)
			{
			case EEditorNotificationType::Success: return Icons::Check;
			case EEditorNotificationType::Warning: return Icons::Warning;
			case EEditorNotificationType::Error: return Icons::Error;
			case EEditorNotificationType::Progress: return Icons::Refresh;
			case EEditorNotificationType::Info:
			default: return Icons::Info;
			}
		}

		auto TypeLabel(EEditorNotificationType Type) -> const char*
		{
			switch (Type)
			{
			case EEditorNotificationType::Success: return "Completed";
			case EEditorNotificationType::Warning: return "Warning";
			case EEditorNotificationType::Error: return "Error";
			case EEditorNotificationType::Progress: return "In progress";
			case EEditorNotificationType::Info:
			default: return "Information";
			}
		}

		auto DrawActionButton(FEditorNotificationManager& Notifications, const FEditorNotification& Notification) -> void
		{
			const ImVec4 Accent = MonaImGui::GetThemeColor(ThemeColor(Notification.Type));
			ImVec4 Button = Accent;
			Button.w = 0.16f;
			ImVec4 Hovered = Accent;
			Hovered.w = 0.28f;
			ImVec4 Active = Accent;
			Active.w = 0.38f;
			ImGui::PushStyleColor(ImGuiCol_Button, Button);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hovered);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, Active);
			ImGui::PushStyleColor(ImGuiCol_Text, Accent);

			if (Notification.Type == EEditorNotificationType::Progress && Notification.Cancel)
			{
				const char* Label = Notification.bCancelRequested ? "Canceling..." : "Cancel";
				ImGui::BeginDisabled(Notification.bCancelRequested);
				if (ImGui::Button(Label)) Notifications.RequestCancel(Notification.Id);
				ImGui::EndDisabled();
			}
			else if (Notification.Action)
			{
				const bool bEnabled = !Notification.Action->IsEnabled || Notification.Action->IsEnabled();
				ImGui::BeginDisabled(!bEnabled);
				if (ImGui::Button(Notification.Action->Label.c_str())) Notifications.InvokeAction(Notification.Id);
				ImGui::EndDisabled();
			}

			ImGui::PopStyleColor(4);
		}

		auto FailureOperation(EEditorTransactionOperation Operation) -> std::string_view
		{
			switch (Operation)
			{
			case EEditorTransactionOperation::Undo: return "undo";
			case EEditorTransactionOperation::Redo: return "redo";
			case EEditorTransactionOperation::Execute:
			default: return "execute";
			}
		}
	}

	auto FEditorNotificationOverlay::Draw(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions, bool* bHistoryOpen) -> void
	{
		PublishTransactionEvents(Notifications, Transactions);
		Notifications.Tick(ImGui::GetIO().DeltaTime);
		if (bHistoryOpen && *bHistoryOpen) DrawHistory(Notifications, bHistoryOpen);

		const std::vector<FEditorNotification>& Active = Notifications.GetNotifications();
		std::vector<const FEditorNotification*> Visible;
		Visible.reserve(std::min(MaxVisibleNotifications, Active.size()));
		for (auto Iterator = Active.rbegin(); Iterator != Active.rend() && Visible.size() < MaxVisibleNotifications; ++Iterator)
		{
			if (Iterator->Type == EEditorNotificationType::Progress) Visible.push_back(&*Iterator);
		}
		for (auto Iterator = Active.rbegin(); Iterator != Active.rend() && Visible.size() < MaxVisibleNotifications; ++Iterator)
		{
			if (Iterator->Type != EEditorNotificationType::Progress) Visible.push_back(&*Iterator);
		}

		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const float Margin = MonaImGui::ScaleUI(Metrics.SpacingL);
		const float Spacing = MonaImGui::ScaleUI(Metrics.SpacingM);
		const float AccentWidth = MonaImGui::ScaleUI(3.0f);
		const float DesiredWidth = MonaImGui::ScaleUI(380.0f);
		const float MinimumWidth = MonaImGui::ScaleUI(180.0f);
		const float AvailableWidth = std::max(MinimumWidth, Viewport->WorkSize.x - Margin * 2.0f);
		const float Width = std::min(DesiredWidth, AvailableWidth);
		float BottomOffset = Margin;

		for (const FEditorNotification* Notification : Visible)
		{
			ImGui::PushID(static_cast<int>(Notification->Id));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(MonaImGui::ScaleUI(Metrics.SpacingL), MonaImGui::ScaleUI(Metrics.SpacingL)));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, MonaImGui::ScaleUI(8.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, MonaImGui::ScaleUI(1.0f));
			ImGui::SetNextWindowViewport(Viewport->ID);
			ImGui::SetNextWindowPos(Viewport->WorkPos + Viewport->WorkSize - ImVec2(Margin, BottomOffset), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
			ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0.0f), ImVec2(Width, std::numeric_limits<float>::max()));
			const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus |
				ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
			const std::string WindowName = std::format("##EditorNotification{}", Notification->Id);
			if (ImGui::Begin(WindowName.c_str(), nullptr, Flags))
			{
				const ImVec4 Accent = MonaImGui::GetThemeColor(ThemeColor(Notification->Type));
				const ImU32 AccentColor = MonaImGui::GetThemeColorU32(ThemeColor(Notification->Type));
				const float CloseSize = ImGui::GetFrameHeight();

				ImGui::PushStyleColor(ImGuiCol_Text, Accent);
				ImGui::Text("%s  %s", TypeIcon(Notification->Type), TypeLabel(Notification->Type));
				ImGui::PopStyleColor();

				// A bare glyph keeps dismissal visually secondary while retaining a generous hit target.
				const ImVec2 CursorAfterHeader = ImGui::GetCursorPos();
				const ImVec2 ClosePos = ImGui::GetWindowPos() + ImVec2(ImGui::GetWindowContentRegionMax().x - CloseSize, ImGui::GetStyle().WindowPadding.y);
				ImGui::SetCursorScreenPos(ClosePos);
				ImGui::InvisibleButton("##Dismiss", ImVec2(CloseSize, CloseSize));
				const bool bCloseHovered = ImGui::IsItemHovered();
				const ImVec2 CloseTextSize = ImGui::CalcTextSize(Icons::Close);
				const ImVec2 CloseTextPos = ClosePos + (ImVec2(CloseSize, CloseSize) - CloseTextSize) * 0.5f;
				const ImU32 CloseColor = ImGui::GetColorU32(bCloseHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
				ImGui::GetWindowDrawList()->AddText(CloseTextPos, CloseColor, Icons::Close);
				if (ImGui::IsItemClicked()) Notifications.Dismiss(Notification->Id);
				ImGui::SetCursorPos(CursorAfterHeader);

				ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
				ImGui::TextUnformatted(Notification->Message.c_str());
				ImGui::PopTextWrapPos();

				if (Notification->Type == EEditorNotificationType::Progress)
				{
					const float Progress = Notification->Progress.value_or(static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0)));
					const std::string Overlay = Notification->Progress ? std::format("{}%", static_cast<int>(*Notification->Progress * 100.0f)) : std::string{};
					ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, MonaImGui::ScaleUI(3.0f));
					ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent);
					ImGui::ProgressBar(Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f), Overlay.empty() ? nullptr : Overlay.c_str());
					ImGui::PopStyleColor();
					ImGui::PopStyleVar();
				}

				if ((Notification->Type == EEditorNotificationType::Progress && Notification->Cancel) || Notification->Action)
				{
					const char* Label = Notification->Type == EEditorNotificationType::Progress ?
						(Notification->bCancelRequested ? "Canceling..." : "Cancel") : Notification->Action->Label.c_str();
					const float ButtonWidth = ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
					ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ButtonWidth);
					DrawActionButton(Notifications, *Notification);
				}

				Notifications.SetHovered(Notification->Id, ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
				const ImVec2 WindowPos = ImGui::GetWindowPos();
				const ImVec2 WindowSize = ImGui::GetWindowSize();
				const float RenderedHeight = std::max(WindowSize.y, ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y);
				ImGui::GetWindowDrawList()->AddRectFilled(WindowPos, WindowPos + ImVec2(AccentWidth, RenderedHeight), AccentColor, ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersLeft);
				BottomOffset += RenderedHeight + Spacing;
			}
			ImGui::End();
			ImGui::PopStyleVar(3);
			ImGui::PopID();
		}
	}

	auto FEditorNotificationOverlay::DrawHistory(FEditorNotificationManager& Notifications, bool* bOpen) -> void
	{
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(520.0f), MonaImGui::ScaleUI(420.0f)), ImGuiCond_FirstUseEver);
		if (EditorWorkspaceUI::BeginDockablePanel(LevelEditorWorkspace::Type, "Activity History", "ActivityHistory", bOpen))
		{
			const std::vector<FEditorNotification>& History = Notifications.GetHistory();
			ImGui::TextDisabled("Editor activity from this session");
			ImGui::SameLine();
			const char* ClearLabel = "Clear history";
			const float ClearWidth = ImGui::CalcTextSize(ClearLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ClearWidth));
			ImGui::BeginDisabled(History.empty());
			if (ImGui::Button(ClearLabel)) Notifications.ClearHistory();
			ImGui::EndDisabled();
			ImGui::Separator();

			if (History.empty())
			{
				ImGui::Spacing();
				ImGui::TextDisabled("Actions, warnings, errors, and background progress will appear here.");
			}
			else
			{
				const bool bDrawEntries = ImGui::BeginChild("##ActivityEntries", ImVec2(0.0f, 0.0f), false);
				if (bDrawEntries)
				{
					for (auto Iterator = History.rbegin(); Iterator != History.rend(); ++Iterator)
					{
						const FEditorNotification& Notification = *Iterator;
						ImGui::PushID(static_cast<int>(Notification.Id));
						const ImVec4 Accent = MonaImGui::GetThemeColor(ThemeColor(Notification.Type));
						ImGui::PushStyleColor(ImGuiCol_Text, Accent);
						ImGui::Text("%s  %s", TypeIcon(Notification.Type), TypeLabel(Notification.Type));
						ImGui::PopStyleColor();
						ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
						ImGui::TextUnformatted(Notification.Message.c_str());
						ImGui::PopTextWrapPos();

						if (Notification.Type == EEditorNotificationType::Progress)
						{
							const float Progress = Notification.Progress.value_or(static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0)));
							const std::string Overlay = Notification.Progress ? std::format("{}%", static_cast<int>(*Notification.Progress * 100.0f)) : std::string{};
							ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent);
							ImGui::ProgressBar(Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f), Overlay.empty() ? nullptr : Overlay.c_str());
							ImGui::PopStyleColor();
						}

						if ((Notification.Type == EEditorNotificationType::Progress && Notification.Cancel) || Notification.Action)
						{
							DrawActionButton(Notifications, Notification);
						}
						ImGui::Separator();
						ImGui::PopID();
					}
				}
				ImGui::EndChild();
			}
		}
		ImGui::End();
	}

	auto FEditorNotificationOverlay::PublishTransactionEvents(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void
	{
		for (FEditorTransactionEvent& Event : Transactions.ConsumeEvents())
		{
			FEditorNotificationDesc Desc;
			if (Event.Type == EEditorTransactionEventType::Failed)
			{
				Desc.Type = EEditorNotificationType::Error;
				Desc.Message = std::format("Failed to {} {}", FailureOperation(Event.Operation), Event.Description);
				Desc.DurationSeconds = 0.0f;
				Notifications.Post(std::move(Desc));
				continue;
			}

			const bool bOffersRedo = Event.Type == EEditorTransactionEventType::Undone;
			Desc.Type = bOffersRedo ? EEditorNotificationType::Info : EEditorNotificationType::Success;
			switch (Event.Type)
			{
			case EEditorTransactionEventType::Undone: Desc.Message = std::format("Undid {}", Event.Description); break;
			case EEditorTransactionEventType::Redone: Desc.Message = std::format("Redid {}", Event.Description); break;
			case EEditorTransactionEventType::Executed:
			default: Desc.Message = Event.Description; break;
			}

			const FEditorTransactionId Id = Event.Id;
			FEditorNotificationAction Action;
			Action.Label = bOffersRedo ? "Redo" : "Undo";
			Action.IsEnabled = [&Transactions, Id, bOffersRedo] {
				return bOffersRedo ? Transactions.IsRedoHead(Id) : Transactions.IsUndoHead(Id);
			};
			Action.Invoke = [&Transactions, Id, bOffersRedo] {
				if (bOffersRedo) Transactions.Redo(Id);
				else Transactions.Undo(Id);
			};
			Desc.Action = std::move(Action);
			Notifications.Post(std::move(Desc));
		}
	}
}
