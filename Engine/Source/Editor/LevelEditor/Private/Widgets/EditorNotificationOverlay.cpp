#include "Widgets/EditorNotificationOverlay.h"

#include "Editor/EditorEngine.h"
#include "Editor/Notification.h"
#include "Editor/Transaction.h"
#include "Editor/WorkspaceUI.h"
#include "AssetForge/ImportService.h"
#include "Icons/FontAwesomeIcons.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "MonaImGui.h"
#include "Profiling/Profiling.h"

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr size_t MaxVisibleNotifications = 5;

		auto ThemeColor(::Durin::Editor::ENotificationType Type) -> MonaImGui::EUIThemeColor
		{
			switch (Type)
			{
			case ::Durin::Editor::ENotificationType::Success: return MonaImGui::EUIThemeColor::Success;
			case ::Durin::Editor::ENotificationType::Warning: return MonaImGui::EUIThemeColor::Warning;
			case ::Durin::Editor::ENotificationType::Error: return MonaImGui::EUIThemeColor::Error;
			case ::Durin::Editor::ENotificationType::Progress:
			case ::Durin::Editor::ENotificationType::Info:
			default: return MonaImGui::EUIThemeColor::Info;
			}
		}

		auto TypeIcon(::Durin::Editor::ENotificationType Type) -> const char*
		{
			switch (Type)
			{
			case ::Durin::Editor::ENotificationType::Success: return Icons::Check;
			case ::Durin::Editor::ENotificationType::Warning: return Icons::Warning;
			case ::Durin::Editor::ENotificationType::Error: return Icons::Error;
			case ::Durin::Editor::ENotificationType::Progress: return Icons::Refresh;
			case ::Durin::Editor::ENotificationType::Info:
			default: return Icons::Info;
			}
		}

		auto TypeLabel(::Durin::Editor::ENotificationType Type) -> const char*
		{
			switch (Type)
			{
			case ::Durin::Editor::ENotificationType::Success: return "Completed";
			case ::Durin::Editor::ENotificationType::Warning: return "Warning";
			case ::Durin::Editor::ENotificationType::Error: return "Error";
			case ::Durin::Editor::ENotificationType::Progress: return "In progress";
			case ::Durin::Editor::ENotificationType::Info:
			default: return "Information";
			}
		}

		auto DrawActionButton(::Durin::Editor::FNotificationManager& Notifications, const ::Durin::Editor::FNotification& Notification) -> void
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

			if (Notification.Type == ::Durin::Editor::ENotificationType::Progress && Notification.Cancel)
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

		auto FailureOperation(::Durin::Editor::ETransactionOperation Operation) -> std::string_view
		{
			switch (Operation)
			{
			case ::Durin::Editor::ETransactionOperation::Undo: return "undo";
			case ::Durin::Editor::ETransactionOperation::Redo: return "redo";
			case ::Durin::Editor::ETransactionOperation::Execute:
			default: return "execute";
			}
		}
	}

	FEditorNotificationOverlay::~FEditorNotificationOverlay()
	{
		if (ImportAggregateNotificationId && GEditor)
			GEditor->GetNotificationManager().Dismiss(ImportAggregateNotificationId);
	}

	auto FEditorNotificationOverlay::Draw(FLevelEditorContext& Context) -> void
	{
		(void)Context;
		if (bFocusHistoryRequested) ImGui::SetNextWindowFocus();
		if (GEditor) DrawHistory(GEditor->GetNotificationManager(), GetOpenPtr());
		bFocusHistoryRequested = false;
	}

	auto FEditorNotificationOverlay::UpdateNotifications(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void
	{
		UpdateImportOperations(Notifications);
		PublishTransactionEvents(Notifications, Transactions);
		Notifications.Tick(ImGui::GetIO().DeltaTime);
	}

	auto FEditorNotificationOverlay::RegisterImportOperation(
		AssetForge::FImportOperationHandle Handle, std::string Title) -> void
	{
		if (!Handle) return;
		const uint64 OperationId = Handle.GetOperationId();
		if (std::ranges::any_of(ImportOperations,
			[OperationId](const FPresentedImportOperation& Operation) {
				return Operation.Handle.GetOperationId() == OperationId;
			})) return;
		ImportOperations.push_back({
			.Handle = std::move(Handle),
			.Title = std::move(Title)});
	}

	auto FEditorNotificationOverlay::UpdateImportOperations(
		::Durin::Editor::FNotificationManager& Notifications) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("AssetImport.NotificationProjection");
		for (FPresentedImportOperation& Operation : ImportOperations)
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("AssetImport.UIPolling");
			const AssetForge::FImportOperationSnapshot Snapshot =
				Operation.Handle.GetSnapshot();
			if (!Operation.NotificationId)
			{
				const AssetForge::FImportOperationHandle CancelHandle = Operation.Handle;
				Operation.NotificationId = Notifications.BeginProgress({
					.Message = std::format("{}: {}", Operation.Title,
						AssetForge::GetImportPhaseLabel(Snapshot.Phase)),
					.Progress = Snapshot.Progress,
					.Cancel = [CancelHandle] { (void)CancelHandle.RequestCancel(); },
					.Presentation = ::Durin::Editor::ENotificationPresentation::HistoryOnly});
			}
			if (Operation.LastRevision == Snapshot.Revision) continue;
			Operation.LastRevision = Snapshot.Revision;
			const std::string Message = std::format("{}: {}", Operation.Title,
				AssetForge::GetImportPhaseLabel(Snapshot.Phase));
			if (!Snapshot.IsTerminal())
			{
				Notifications.UpdateProgress(
					Operation.NotificationId, Snapshot.Progress, Message);
				continue;
			}
			switch (Snapshot.State)
			{
			case AssetForge::EImportOperationState::Succeeded:
				Notifications.CompleteProgress(Operation.NotificationId,
					std::format("{} completed", Operation.Title));
				break;
			case AssetForge::EImportOperationState::Canceled:
			case AssetForge::EImportOperationState::Superseded:
				Notifications.CancelProgress(Operation.NotificationId,
					Snapshot.State == AssetForge::EImportOperationState::Superseded
						? std::format("{} superseded", Operation.Title)
						: std::format("{} canceled", Operation.Title));
				break;
			case AssetForge::EImportOperationState::Failed:
			case AssetForge::EImportOperationState::Rejected:
				Notifications.FailProgress(Operation.NotificationId,
					Snapshot.Diagnostic.empty()
						? std::format("{} failed", Operation.Title)
						: Snapshot.Diagnostic);
				break;
			default: break;
			}
		}

		std::erase_if(ImportOperations, [](const FPresentedImportOperation& Operation) {
			return Operation.Handle.GetSnapshot().IsTerminal();
		});
		if (ImportOperations.empty())
		{
			if (ImportAggregateNotificationId)
			{
				Notifications.Dismiss(ImportAggregateNotificationId);
				ImportAggregateNotificationId = 0;
			}
			return;
		}

		const AssetForge::FImportOperationSnapshot Primary =
			ImportOperations.front().Handle.GetSnapshot();
		const std::string AggregateMessage = ImportOperations.size() == 1
			? std::format("{}: {}", ImportOperations.front().Title,
				AssetForge::GetImportPhaseLabel(Primary.Phase))
			: std::format("{}: {} ({} active)", ImportOperations.front().Title,
				AssetForge::GetImportPhaseLabel(Primary.Phase), ImportOperations.size());
		std::optional<float> AggregateProgress = Primary.Progress;
		if (ImportOperations.size() > 1) AggregateProgress.reset();
		if (!ImportAggregateNotificationId)
		{
			ImportAggregateNotificationId = Notifications.BeginProgress({
				.Message = AggregateMessage,
				.Progress = AggregateProgress,
				.Action = ::Durin::Editor::FNotificationAction{
					.Label = "Details",
					.Invoke = [this] { OpenHistory(); }},
				.Presentation = ::Durin::Editor::ENotificationPresentation::StatusBar,
				.bRecordInHistory = false});
		}
		else Notifications.UpdateProgress(
			ImportAggregateNotificationId, AggregateProgress, AggregateMessage);
	}

	auto FEditorNotificationOverlay::GetStatusBarHeight() const -> float
	{
		return ImGui::GetFrameHeight() + MonaImGui::ScaleUI(4.0f);
	}

	auto FEditorNotificationOverlay::DrawStatusBar(
		::Durin::Editor::FNotificationManager& Notifications,
		EEditorStatusBarAction SelectedDrawer,
		uint32 ConsoleUnreadCount) -> EEditorStatusBarAction
	{
		EEditorStatusBarAction Result = EEditorStatusBarAction::None;
		const float Height = GetStatusBarHeight();
		const float SeparatorSize = ImGui::GetStyle().SeparatorSize;
		const ImVec2 StatusPadding(ImGui::GetStyle().WindowPadding.x, MonaImGui::ScaleUI(2.0f));
		const ImVec4 Transparent(0.0f, 0.0f, 0.0f, 0.0f);
		ImVec4 ToolHovered = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
		ToolHovered.w *= 0.48f;
		ImVec4 ToolActive = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
		ToolActive.w *= 0.62f;
		ImVec4 ToolSelected = ImGui::GetStyleColorVec4(ImGuiCol_Header);
		ToolSelected.w *= 0.72f;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, StatusPadding);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
			ImVec2(ImGui::GetStyle().CellPadding.x, 0.0f));
		if (ImGui::BeginChild("##EditorStatusBar", ImVec2(0.0f, Height), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			const ImVec2 BarMin = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(
				BarMin,
				ImVec2(BarMin.x + ImGui::GetWindowWidth(), BarMin.y + SeparatorSize),
				ImGui::GetColorU32(ImGuiCol_Separator));
			const ::Durin::Editor::FNotification* Status = Notifications.GetStatusNotification()
				? &*Notifications.GetStatusNotification() : nullptr;
			const bool bCompact = ImGui::GetContentRegionAvail().x < MonaImGui::ScaleUI(760.0f);
			const std::string ContentLabel = std::format("{}{}###EditorContentDrawer", Icons::FolderOpen,
				bCompact ? "" : "  Content Browser");
			const std::string ConsoleLabel = ConsoleUnreadCount == 0
				? std::format("{}{}###EditorConsole", Icons::Terminal,
					bCompact ? "" : "  Console")
				: std::format("{}{}  {}###EditorConsole", Icons::Terminal,
					bCompact ? "" : "  Console", ConsoleUnreadCount);
			const std::string ActivityLabel = std::format("{}###EditorActivityHistory", Icons::List);
			const float ContentWidth = ImGui::CalcTextSize(ContentLabel.c_str(), nullptr, true).x
				+ ImGui::GetStyle().FramePadding.x * 2.0f;
			const float ConsoleWidth = ImGui::CalcTextSize(ConsoleLabel.c_str(), nullptr, true).x
				+ ImGui::GetStyle().FramePadding.x * 2.0f;
			const float ActivityWidth = ImGui::CalcTextSize(ActivityLabel.c_str(), nullptr, true).x
				+ ImGui::GetStyle().FramePadding.x * 2.0f;
			const float StatusWidth = Status
				? std::min(
					ImGui::CalcTextSize(TypeIcon(Status->Type)).x + ImGui::GetStyle().ItemSpacing.x
						+ ImGui::CalcTextSize(Status->Message.c_str()).x,
					ImGui::GetContentRegionAvail().x * 0.45f)
				: ImGui::CalcTextSize("Ready").x;
			const float ActionWidth = Status && Status->Action
				? ImGui::CalcTextSize(Status->Action->Label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;

			auto DrawToolButton = [&](const std::string& Label, const char* Tooltip,
				EEditorStatusBarAction Action, float Width = 0.0f) {
				const bool bSelected = SelectedDrawer == Action;
				ImGui::PushStyleColor(ImGuiCol_Button, bSelected ? ToolSelected : Transparent);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToolHovered);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToolActive);
				if (ImGui::Button(Label.c_str(), ImVec2(Width, 0.0f))) Result = Action;
				ImGui::PopStyleColor(3);
				if (bSelected)
				{
					const ImVec2 ItemMin = ImGui::GetItemRectMin();
					const ImVec2 ItemMax = ImGui::GetItemRectMax();
					const float IndicatorHeight = MonaImGui::ScaleUI(2.0f);
					ImGui::GetWindowDrawList()->AddRectFilled(
						ImVec2(ItemMin.x, ItemMax.y - IndicatorHeight),
						ItemMax,
						ImGui::ColorConvertFloat4ToU32(
							MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::SelectionSecondary)),
						0.0f);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tooltip);
			};

			if (ImGui::BeginTable("##EditorStatusLayout", 5,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("DrawerTools", ImGuiTableColumnFlags_WidthFixed,
					ContentWidth + ConsoleWidth);
				ImGui::TableSetupColumn("Spacer", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, StatusWidth);
				ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, ActionWidth);
				ImGui::TableSetupColumn("Activity", ImGuiTableColumnFlags_WidthFixed, ActivityWidth);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
					ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
				DrawToolButton(ContentLabel, "Content Browser (Ctrl+Space)",
					EEditorStatusBarAction::ContentBrowser, ContentWidth);

				ImGui::SameLine(0.0f, 0.0f);
				if (ConsoleUnreadCount > 0)
					ImGui::PushStyleColor(ImGuiCol_Text,
						MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error));
				DrawToolButton(ConsoleLabel, ConsoleUnreadCount == 0
					? "Console" : "Console has unread warnings or errors",
					EEditorStatusBarAction::Console, ConsoleWidth);
				if (ConsoleUnreadCount > 0) ImGui::PopStyleColor();
				ImGui::PopStyleVar();

				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				if (Status)
				{
					const ImVec2 ProgressMin = ImGui::GetCursorScreenPos();
					const float ProgressWidth = ImGui::GetContentRegionAvail().x;
					const ImVec4 Accent = MonaImGui::GetThemeColor(ThemeColor(Status->Type));
					ImGui::PushStyleColor(ImGuiCol_Text, Accent);
					ImGui::TextUnformatted(TypeIcon(Status->Type));
					ImGui::PopStyleColor();
					ImGui::SameLine();
					ImGui::TextUnformatted(Status->Message.c_str());
					if (Status->Type == ::Durin::Editor::ENotificationType::Progress
						&& ProgressWidth > 0.0f)
					{
						const float IndicatorHeight = MonaImGui::ScaleUI(2.0f);
						const float IndicatorY = ProgressMin.y + ImGui::GetFrameHeight()
							- IndicatorHeight;
						ImGui::GetWindowDrawList()->AddRectFilled(
							ImVec2(ProgressMin.x, IndicatorY),
							ImVec2(ProgressMin.x + ProgressWidth, IndicatorY + IndicatorHeight),
							ImGui::GetColorU32(ImGuiCol_FrameBg));
						float Start = 0.0f;
						float End = Status->Progress.value_or(0.0f);
						if (!Status->Progress)
						{
							Start = static_cast<float>(std::fmod(ImGui::GetTime() * 0.55, 1.15));
							End = std::min(Start + 0.28f, 1.0f);
							Start = std::min(Start, 1.0f);
						}
						ImGui::GetWindowDrawList()->AddRectFilled(
							ImVec2(ProgressMin.x + ProgressWidth * Start, IndicatorY),
							ImVec2(ProgressMin.x + ProgressWidth * End,
								IndicatorY + IndicatorHeight),
							ImGui::ColorConvertFloat4ToU32(Accent));
					}
				}
				else ImGui::TextDisabled("Ready");

				ImGui::TableNextColumn();
				if (Status && Status->Action) DrawActionButton(Notifications, *Status);

				ImGui::TableNextColumn();
				DrawToolButton(ActivityLabel, "Activity History",
					EEditorStatusBarAction::ActivityHistory);
				ImGui::EndTable();
			}
			if (Status) Notifications.SetHovered(Status->Id, ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(5);
		ImGui::PopStyleColor();
		return Result;
	}

	auto FEditorNotificationOverlay::OpenHistory() -> void
	{
		SetOpen(true);
		bFocusHistoryRequested = true;
	}

	auto FEditorNotificationOverlay::DrawToasts(::Durin::Editor::FNotificationManager& Notifications) -> void
	{

		const std::vector<::Durin::Editor::FNotification>& Active = Notifications.GetNotifications();
		std::vector<const ::Durin::Editor::FNotification*> Visible;
		Visible.reserve(std::min(MaxVisibleNotifications, Active.size()));
		for (auto Iterator = Active.rbegin(); Iterator != Active.rend() && Visible.size() < MaxVisibleNotifications; ++Iterator)
		{
			if (Iterator->Type == ::Durin::Editor::ENotificationType::Progress) Visible.push_back(&*Iterator);
		}
		for (auto Iterator = Active.rbegin(); Iterator != Active.rend() && Visible.size() < MaxVisibleNotifications; ++Iterator)
		{
			if (Iterator->Type != ::Durin::Editor::ENotificationType::Progress) Visible.push_back(&*Iterator);
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

		for (const ::Durin::Editor::FNotification* Notification : Visible)
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

				if (Notification->Type == ::Durin::Editor::ENotificationType::Progress)
				{
					const float Progress = Notification->Progress.value_or(static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0)));
					const std::string Overlay = Notification->Progress ? std::format("{}%", static_cast<int>(*Notification->Progress * 100.0f)) : std::string{};
					ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, MonaImGui::ScaleUI(3.0f));
					ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent);
					ImGui::ProgressBar(Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f), Overlay.empty() ? nullptr : Overlay.c_str());
					ImGui::PopStyleColor();
					ImGui::PopStyleVar();
				}

				if ((Notification->Type == ::Durin::Editor::ENotificationType::Progress && Notification->Cancel) || Notification->Action)
				{
					const char* Label = Notification->Type == ::Durin::Editor::ENotificationType::Progress ?
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

	auto FEditorNotificationOverlay::DrawHistory(::Durin::Editor::FNotificationManager& Notifications, bool* bOpen) -> void
	{
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(520.0f), MonaImGui::ScaleUI(420.0f)), ImGuiCond_FirstUseEver);
		if (::Durin::Editor::WorkspaceUI::BeginDockablePanel(
			Workspace::Type, "Activity History", "ActivityHistory", bOpen,
			ImGuiWindowFlags_NoDocking))
		{
			const std::vector<::Durin::Editor::FNotification>& History = Notifications.GetHistory();
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
						const ::Durin::Editor::FNotification& Notification = *Iterator;
						ImGui::PushID(static_cast<int>(Notification.Id));
						const ImVec4 Accent = MonaImGui::GetThemeColor(ThemeColor(Notification.Type));
						ImGui::PushStyleColor(ImGuiCol_Text, Accent);
						ImGui::Text("%s  %s", TypeIcon(Notification.Type), TypeLabel(Notification.Type));
						ImGui::PopStyleColor();
						ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
						ImGui::TextUnformatted(Notification.Message.c_str());
						ImGui::PopTextWrapPos();
						if (!Notification.Details.empty())
						{
							ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
							ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
							ImGui::TextUnformatted(Notification.Details.c_str());
							ImGui::PopTextWrapPos();
							ImGui::PopStyleColor();
						}

						if (Notification.Type == ::Durin::Editor::ENotificationType::Progress)
						{
							const float Progress = Notification.Progress.value_or(static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0)));
							const std::string Overlay = Notification.Progress ? std::format("{}%", static_cast<int>(*Notification.Progress * 100.0f)) : std::string{};
							ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent);
							ImGui::ProgressBar(Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f), Overlay.empty() ? nullptr : Overlay.c_str());
							ImGui::PopStyleColor();
						}

						if ((Notification.Type == ::Durin::Editor::ENotificationType::Progress && Notification.Cancel) || Notification.Action)
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

	auto FEditorNotificationOverlay::PublishTransactionEvents(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void
	{
		for (::Durin::Editor::FTransactionEvent& Event : Transactions.ConsumeEvents())
		{
			::Durin::Editor::FNotificationDesc Desc;
			if (Event.Type == ::Durin::Editor::ETransactionEventType::Failed)
			{
				Desc.Type = ::Durin::Editor::ENotificationType::Error;
				Desc.Message = std::format("Failed to {} {}", FailureOperation(Event.Operation), Event.Description);
				Desc.Details = std::move(Event.Details);
				Desc.DurationSeconds = 0.0f;
				Notifications.Post(std::move(Desc));
				continue;
			}

			const bool bOffersRedo = Event.Type == ::Durin::Editor::ETransactionEventType::Undone;
			Desc.Type = bOffersRedo ? ::Durin::Editor::ENotificationType::Info : ::Durin::Editor::ENotificationType::Success;
			switch (Event.Type)
			{
			case ::Durin::Editor::ETransactionEventType::Undone: Desc.Message = std::format("Undid {}", Event.Description); break;
			case ::Durin::Editor::ETransactionEventType::Redone: Desc.Message = std::format("Redid {}", Event.Description); break;
			case ::Durin::Editor::ETransactionEventType::Executed:
			default: Desc.Message = Event.Description; break;
			}
			Desc.Details = std::move(Event.Details);
			Desc.Presentation = ::Durin::Editor::ENotificationPresentation::StatusBar;

			const ::Durin::Editor::FTransactionId Id = Event.Id;
			::Durin::Editor::FNotificationAction Action;
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
