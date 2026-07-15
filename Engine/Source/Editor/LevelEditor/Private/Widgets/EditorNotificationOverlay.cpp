#include "Widgets/EditorNotificationOverlay.h"

#include "Editor/EditorNotification.h"
#include "Editor/EditorTransaction.h"
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

		auto TypeMarker(EEditorNotificationType Type) -> const char*
		{
			switch (Type)
			{
			case EEditorNotificationType::Success: return "OK";
			case EEditorNotificationType::Warning: return "!";
			case EEditorNotificationType::Error: return "X";
			case EEditorNotificationType::Progress: return ">";
			case EEditorNotificationType::Info:
			default: return "i";
			}
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

	auto FEditorNotificationOverlay::Draw(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void
	{
		PublishTransactionEvents(Notifications, Transactions);
		Notifications.Tick(ImGui::GetIO().DeltaTime);

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
		const float AccentWidth = MonaImGui::ScaleUI(4.0f);
		const float DesiredWidth = MonaImGui::ScaleUI(360.0f);
		const float MinimumWidth = MonaImGui::ScaleUI(180.0f);
		const float AvailableWidth = std::max(MinimumWidth, Viewport->WorkSize.x - Margin * 2.0f);
		const float Width = std::min(DesiredWidth, AvailableWidth);
		float BottomOffset = Margin;

		for (const FEditorNotification* Notification : Visible)
		{
			ImGui::PushID(static_cast<int>(Notification->Id));
			ImGui::SetNextWindowViewport(Viewport->ID);
			ImGui::SetNextWindowPos(Viewport->WorkPos + Viewport->WorkSize - ImVec2(Margin, BottomOffset), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
			ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0.0f), ImVec2(Width, std::numeric_limits<float>::max()));
			const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus |
				ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
			const std::string WindowName = std::format("##EditorNotification{}", Notification->Id);
			if (ImGui::Begin(WindowName.c_str(), nullptr, Flags))
			{
				const ImU32 AccentColor = MonaImGui::GetThemeColorU32(ThemeColor(Notification->Type));

				const float CloseWidth = ImGui::GetFrameHeight();
				ImGui::PushStyleColor(ImGuiCol_Text, AccentColor);
				ImGui::TextUnformatted(TypeMarker(Notification->Type));
				ImGui::PopStyleColor();
				ImGui::SameLine();
				const float MessageStart = ImGui::GetCursorPosX();
				ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - CloseWidth - Spacing);
				ImGui::TextUnformatted(Notification->Message.c_str());
				ImGui::PopTextWrapPos();

				ImGui::SameLine();
				ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - CloseWidth);
				if (ImGui::SmallButton("x")) Notifications.Dismiss(Notification->Id);

				if (Notification->Type == EEditorNotificationType::Progress)
				{
					const float Progress = Notification->Progress.value_or(static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0)));
					const std::string Overlay = Notification->Progress ? std::format("{}%", static_cast<int>(*Notification->Progress * 100.0f)) : std::string{};
					ImGui::ProgressBar(Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f), Overlay.empty() ? nullptr : Overlay.c_str());
					if (Notification->Cancel)
					{
						const char* Label = Notification->bCancelRequested ? "Canceling..." : "Cancel";
						const float ButtonWidth = ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
						ImGui::SetCursorPosX(std::max(MessageStart, ImGui::GetWindowContentRegionMax().x - ButtonWidth));
						ImGui::BeginDisabled(Notification->bCancelRequested);
						if (ImGui::Button(Label)) Notifications.RequestCancel(Notification->Id);
						ImGui::EndDisabled();
					}
				}
				else if (Notification->Action)
				{
					const bool bEnabled = !Notification->Action->IsEnabled || Notification->Action->IsEnabled();
					const float ButtonWidth = ImGui::CalcTextSize(Notification->Action->Label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
					ImGui::SetCursorPosX(std::max(MessageStart, ImGui::GetWindowContentRegionMax().x - ButtonWidth));
					ImGui::BeginDisabled(!bEnabled);
					if (ImGui::Button(Notification->Action->Label.c_str())) Notifications.InvokeAction(Notification->Id);
					ImGui::EndDisabled();
				}

				Notifications.SetHovered(Notification->Id, ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
				const ImVec2 WindowPos = ImGui::GetWindowPos();
				const ImVec2 WindowSize = ImGui::GetWindowSize();
				const float RenderedHeight = std::max(WindowSize.y, ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y);
				ImGui::GetWindowDrawList()->AddRectFilled(WindowPos, WindowPos + ImVec2(AccentWidth, RenderedHeight), AccentColor, ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersLeft);
				BottomOffset += RenderedHeight + Spacing;
			}
			ImGui::End();
			ImGui::PopID();
		}
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
