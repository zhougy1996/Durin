#include "Panels/ConsolePanel.h"

#include "Panels/ConsolePanelLayout.h"
#include "Panels/ConsoleRecordModel.h"

#include "Icons/FontAwesomeIcons.h"
#include "Logging/Logger.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin::Editor::MainFrame
{
	// Retains console input and filtering state across panel reconstruction.
	struct FConsolePanelState
	{
		std::atomic<bool> bClearRequested = false;
		FConsoleRecordModel Records;
	};

	namespace
	{
		using StringUtils::ContainsInsensitive;

		constexpr size_t MaxCommandHistory = 100;

		auto DrawToolbarIconButton(const char* Icon, const char* Id) -> bool
		{
			return MonaImGui::ToolbarIconButton(Icon, Id);
		}

		constexpr auto AccumulateConsoleUnreadImportantRecord(
			uint32 CurrentCount, ELogLevel Level) -> uint32
		{
			if (Level < ELogLevel::Warn) return CurrentCount;
			return CurrentCount >= 999 ? 999 : CurrentCount + 1;
		}

		auto LevelName(ELogLevel Level) -> const char*
		{
			switch (Level)
			{
			case ELogLevel::Trace: return "Trace";
			case ELogLevel::Debug: return "Debug";
			case ELogLevel::Info: return "Info";
			case ELogLevel::Warn: return "Warn";
			case ELogLevel::Error: return "Error";
			case ELogLevel::Fatal: return "Fatal";
			default: return "Unknown";
			}
		}

		auto LevelInitial(ELogLevel Level) -> const char*
		{
			switch (Level)
			{
			case ELogLevel::Trace: return "T";
			case ELogLevel::Debug: return "D";
			case ELogLevel::Info: return "I";
			case ELogLevel::Warn: return "W";
			case ELogLevel::Error: return "E";
			case ELogLevel::Fatal: return "F";
			default: return "?";
			}
		}

		auto LevelColor(ELogLevel Level) -> ImVec4
		{
			switch (Level)
			{
			case ELogLevel::Trace: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
			case ELogLevel::Debug: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info);
			case ELogLevel::Info: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Success);
			case ELogLevel::Warn: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning);
			case ELogLevel::Error: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error);
			case ELogLevel::Fatal: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error);
			default: return {1.0f, 1.0f, 1.0f, 1.0f};
			}
		}

		auto FormatTime(const FLogRecord& Record) -> std::array<char, 16>
		{
			const std::time_t Time = std::chrono::system_clock::to_time_t(Record.Timestamp);
			std::tm LocalTime{};
#ifdef _WIN32
			localtime_s(&LocalTime, &Time);
#else
			localtime_r(&Time, &LocalTime);
#endif
			std::array<char, 16> TimeText{};
			std::strftime(TimeText.data(), TimeText.size(), "%H:%M:%S", &LocalTime);
			return TimeText;
		}

		auto FormatLog(const FLogRecord& Record) -> std::string
		{
			const std::array<char, 16> TimeText = FormatTime(Record);
			return std::format("[{}][{}][{}] {}", TimeText.data(), LevelName(Record.Level), Record.GetCategory(), Record.Message);
		}

		auto MeasureLogPrefixWidth(const FLogRecord& Record) -> float
		{
			const float Spacing = MonaImGui::GetUIStyleMetrics().SpacingS;
			const std::array<char, 16> TimeText = FormatTime(Record);
			const std::string Category = std::format("[{}]", Record.GetCategory());
			return ImGui::CalcTextSize(TimeText.data()).x + Spacing
				+ ImGui::CalcTextSize("W").x + Spacing
				+ ImGui::CalcTextSize(Category.c_str()).x + Spacing;
		}

		auto DrawLogRecord(const FLogRecord& Record) -> void
		{
			const float ContentWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
			const std::array<char, 16> TimeText = FormatTime(Record);
			const float Spacing = MonaImGui::GetUIStyleMetrics().SpacingS;
			ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::ConsoleTimestamp), "%s", TimeText.data());
			ImGui::SameLine(0.0f, Spacing);
			const float LevelStartX = ImGui::GetCursorPosX();
			const char* LevelText = LevelInitial(Record.Level);
			const float LevelSlotWidth = ImGui::CalcTextSize("W").x;
			const float LevelTextWidth = ImGui::CalcTextSize(LevelText).x;
			ImGui::SetCursorPosX(LevelStartX + (LevelSlotWidth - LevelTextWidth) * 0.5f);
			const ImVec4 Color = LevelColor(Record.Level);
			ImGui::TextColored(Color, "%s", LevelText);
			// A subtle second pass gives narrow glyphs such as "I" enough weight to read as a level marker.
			ImGui::GetWindowDrawList()->AddText(
				ImGui::GetItemRectMin() + ImVec2(MonaImGui::ScaleUI(0.6f), 0.0f), ImGui::ColorConvertFloat4ToU32(Color), LevelText);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", LevelName(Record.Level));
			ImGui::SameLine(LevelStartX + LevelSlotWidth, Spacing);
			const std::string_view Category = Record.GetCategory();
			ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::ConsoleModule), "[%.*s]",
				static_cast<int>(Category.size()), Category.data());
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			{
				ImGui::BeginTooltip();
				ImGui::Text("Module: %s", Record.Module.c_str());
				if (!Record.ThreadName.empty())
					ImGui::Text("Thread: %s (%u)", Record.ThreadName.c_str(), Record.ThreadId);
				else if (Record.ThreadId != 0)
					ImGui::Text("Thread: %u", Record.ThreadId);
				if (!Record.File.empty()) ImGui::Text("Source: %s:%u", Record.File.c_str(), Record.Line);
				ImGui::EndTooltip();
			}
			const float RemainingWidth = ContentWidth - MeasureLogPrefixWidth(Record);
			const float MinimumInlineMessageWidth = std::min(
				MonaImGui::ScaleUI(160.0f), ContentWidth * 0.45f);
			if (RemainingWidth >= MinimumInlineMessageWidth)
				ImGui::SameLine(0.0f, Spacing);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(Record.Message.c_str());
			ImGui::PopTextWrapPos();
		}

		auto MeasureLogRecordHeight(const FLogRecord& Record, float ContentWidth) -> float
		{
			const float PrefixWidth = MeasureLogPrefixWidth(Record);
			const float MessageWidth = std::max(ImGui::GetFontSize(), ContentWidth - PrefixWidth);
			const float MinimumInlineMessageWidth = std::min(
				MonaImGui::ScaleUI(160.0f), ContentWidth * 0.45f);
			if (ContentWidth - PrefixWidth >= MinimumInlineMessageWidth)
				return std::max(ImGui::GetTextLineHeight(),
					ImGui::CalcTextSize(Record.Message.c_str(), nullptr, false, MessageWidth).y);
			return ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y
				+ std::max(ImGui::GetTextLineHeight(),
					ImGui::CalcTextSize(Record.Message.c_str(), nullptr, false, ContentWidth).y);
		}

		auto MeasureConsoleRecordHeight(const FConsoleRecord& Record, float ContentWidth) -> float
		{
			if (Record.Type == EConsoleRecordType::Log)
				return MeasureLogRecordHeight(Record.Log, ContentWidth);
			return std::max(ImGui::GetTextLineHeight(),
				ImGui::CalcTextSize(Record.Text.c_str(), nullptr, false,
					std::max(ImGui::GetFontSize(), ContentWidth)).y);
		}

		auto DrawConsoleRecord(const FConsoleRecord& Record) -> void
		{
			if (Record.Type == EConsoleRecordType::Log)
			{
				DrawLogRecord(Record.Log);
				return;
			}
			const ImVec4 Color = Record.Type == EConsoleRecordType::Command ? MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info) :
				Record.Type == EConsoleRecordType::HistoryGap ? MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning) :
				Record.Type == EConsoleRecordType::Error ? MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error) :
				ImGui::GetStyleColorVec4(ImGuiCol_Text);
			ImGui::PushStyleColor(ImGuiCol_Text, Color);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(Record.Text.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}

		auto CommonPrefix(const std::vector<std::string>& Values) -> std::string
		{
			if (Values.empty()) return {};
			std::string Prefix = Values.front();
			for (size_t Index = 1; Index < Values.size(); ++Index)
			{
				size_t Length = 0;
				while (Length < Prefix.size() && Length < Values[Index].size() && std::tolower(static_cast<unsigned char>(Prefix[Length])) == std::tolower(static_cast<unsigned char>(Values[Index][Length])))
					++Length;
				Prefix.resize(Length);
			}
			return Prefix;
		}
	} // namespace

	FConsolePanel::FConsolePanel(FModuleOwnedCallbackGate OwnerGate)
		: State(std::make_shared<FConsolePanelState>())
	{
		const std::weak_ptr<FConsolePanelState> WeakState = State;
		ClearCommandHandle = FConsoleCommandRegistry::Get().RegisterCommand({"clear", "Clears the editor console.", "clear", [WeakState](std::span<const std::string> Args) {
																				 if (!Args.empty()) return FConsoleCommandResult::Failure("Usage: clear");
																				 if (const auto SharedState = WeakState.lock())
																				 {
																					 SharedState->bClearRequested.store(true, std::memory_order_release);
																				 }
																				 return FConsoleCommandResult::Success();
														 }}, std::move(OwnerGate));
	}

	FConsolePanel::~FConsolePanel()
	{
		FConsoleCommandRegistry::Get().UnregisterCommand(ClearCommandHandle);
		State.reset();
	}

	auto FConsolePanel::Tick() -> void
	{
		bHasNewConsoleRecords = PollLogRecords(true) || bHasNewConsoleRecords;
	}

	auto FConsolePanel::DrawContents() -> void
	{
		const bool bHadNewConsoleRecords = std::exchange(bHasNewConsoleRecords, false);
		UnreadImportantRecordCount = 0;
		DrawBody(bHadNewConsoleRecords);
	}

	auto FConsolePanel::DrawBody(bool bReceivedRecords) -> void
	{
		if (DrawToolbarIconButton(Icons::Gear, "ConsoleOptionsButton")) ImGui::OpenPopup("ConsoleOptions");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Options");
		if (ImGui::BeginPopup("ConsoleOptions"))
		{
			ImGui::MenuItem("Auto Scroll", nullptr, &bAutoScroll);
			if (ImGui::BeginMenu("Levels"))
			{
				for (size_t Index = 0; Index < LevelVisibility.size(); ++Index)
				{
					const ELogLevel Level = static_cast<ELogLevel>(Index);
					ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(Level));
					if (ImGui::MenuItem(LevelName(Level), nullptr, &LevelVisibility[Index])) bVisibleRecordsDirty = true;
					ImGui::PopStyleColor();
				}
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (DrawToolbarIconButton(Icons::ArrowDown, "ConsoleScrollToLatest")) RequestScrollToLatest();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Scroll to latest record");
		ImGui::SameLine();
		if (DrawToolbarIconButton(Icons::Copy, "ConsoleCopyVisible")) CopyVisibleRecords();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Copy visible records");
		ImGui::SameLine();
		if (DrawToolbarIconButton(Icons::Trash, "ConsoleClear")) Clear();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Clear console");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputTextWithHint("###ConsoleSearch", "Search output...", SearchText.data(), SearchText.size())) bVisibleRecordsDirty = true;
		ImGui::Separator();

		const float InputHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		if (ImGui::BeginChild("ConsoleRecords", ImVec2(0, -InputHeight)))
		{
			// Sample after ImGui has applied this frame's wheel/scrollbar input, but before
			// new records change the content extent, so scrolling up immediately breaks follow mode.
			const bool bIsAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
			RefreshVisibleRecords();
			const float ContentWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
			const bool bLayoutChanged = RefreshVisibleRecordLayout(ContentWidth);
			if (VisibleRecordIndices.empty())
			{
				ImGui::TextDisabled("%s", State->Records.GetRecords().empty() ? "No console records." : "No records match the current filters.");
			}
			else
			{
				const bool bShouldFollow = ScrollToLatestFrames > 0
					|| (bAutoScroll && bIsAtBottom && (bReceivedRecords || bLayoutChanged));
				const float RecordsStartY = ImGui::GetCursorPosY();
				const FConsoleVisibleRange Range = ResolveConsoleVisibleRange(
					VisibleRecordOffsets, ImGui::GetScrollY(), ImGui::GetWindowHeight());
				for (size_t VisibleIndex = Range.Begin; VisibleIndex < Range.End; ++VisibleIndex)
				{
					ImGui::SetCursorPosY(RecordsStartY + VisibleRecordOffsets[VisibleIndex]);
					DrawConsoleRecord(State->Records.GetRecords()[VisibleRecordIndices[VisibleIndex]]);
				}
				ImGui::SetCursorPosY(RecordsStartY + VisibleRecordOffsets.back());
				ImGui::Dummy(ImVec2(0.0f, 0.0f));
				if (bShouldFollow) ImGui::SetScrollHereY(1.0f);
			}
			if (ScrollToLatestFrames > 0) --ScrollToLatestFrames;
			if (ImGui::BeginPopupContextWindow("ConsoleRecordsContext", ImGuiPopupFlags_MouseButtonRight))
			{
				if (ImGui::MenuItem("Copy Visible")) CopyVisibleRecords();
				if (ImGui::MenuItem("Clear")) Clear();
				ImGui::EndPopup();
			}
		}
		ImGui::EndChild();

		ImGui::Separator();
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info), ">");
		ImGui::SameLine(0.0f, MonaImGui::GetUIStyleMetrics().SpacingM);
		ImGui::SetNextItemWidth(-1.0f);
		const ImGuiInputTextFlags Flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
		if (bRefocusInput)
		{
			ImGui::SetKeyboardFocusHere();
			bRefocusInput = false;
		}
		if (ImGui::InputTextWithHint("###ConsoleInput", "Command (Enter: run, Tab: autocomplete, Up/Down: history)", CommandText.data(), CommandText.size(), Flags, InputTextCallback, this))
		{
			ExecuteCommand(CommandText.data());
			CommandText.fill('\0');
			bRefocusInput = true;
		}
	}

	auto FConsolePanel::InputTextCallback(ImGuiInputTextCallbackData* Data) -> int
	{
		auto* Panel = static_cast<FConsolePanel*>(Data->UserData);
		if (Data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
		{
			const int PreviousPosition = Panel->HistoryPosition;
			if (Data->EventKey == ImGuiKey_UpArrow)
			{
				if (Panel->HistoryPosition == -1 && !Panel->History.empty())
					Panel->HistoryPosition = static_cast<int>(Panel->History.size()) - 1;
				else if (Panel->HistoryPosition > 0)
					--Panel->HistoryPosition;
			}
			else if (Data->EventKey == ImGuiKey_DownArrow && Panel->HistoryPosition != -1)
			{
				if (++Panel->HistoryPosition >= static_cast<int>(Panel->History.size())) Panel->HistoryPosition = -1;
			}
			if (PreviousPosition != Panel->HistoryPosition)
			{
				const char* Text = Panel->HistoryPosition >= 0 ? Panel->History[Panel->HistoryPosition].c_str() : "";
				Data->DeleteChars(0, Data->BufTextLen);
				Data->InsertChars(0, Text);
			}
		}
		else if (Data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			const std::string_view Input(Data->Buf, static_cast<size_t>(Data->CursorPos));
			const size_t Space = Input.find_first_of(" \t");
			if (Space != std::string_view::npos) return 0;
			const std::vector<std::string> Matches = FConsoleCommandRegistry::Get().FindCompletions(Input);
			if (Matches.empty()) return 0;
			const std::string Completion = Matches.size() == 1 ? Matches.front() + " " : CommonPrefix(Matches);
			if (Completion.size() > Input.size()) Data->InsertChars(Data->CursorPos, Completion.data() + Input.size(), Completion.data() + Completion.size());
			if (Matches.size() > 1)
			{
				std::string Text = "Matches:";
				for (const std::string& Match : Matches)
					Text += " " + Match;
				Panel->State->Records.AddText(EConsoleRecordType::Result, std::move(Text));
				Panel->MarkRecordsChanged();
				Panel->bHasNewConsoleRecords = true;
				Panel->ScrollToLatestFrames = 2;
			}
		}
		return 0;
	}

	auto FConsolePanel::PollLogRecords(bool bCountUnread) -> bool
	{
		bool bChanged = ApplyPendingRequests();
		FLogReadResult Read = FLogger::Get().ReadRecords(NextLogSequence);
		// Being exactly one past the newest sequence means the reader is caught up, not that the logger restarted.
		if (Read.NewestAvailableSequence != 0 && NextLogSequence > 1 && Read.NewestAvailableSequence < NextLogSequence - 1)
		{
			State->Records.Clear();
			NextLogSequence = 1;
			EvictedLogRecordCount = 0;
			Read = FLogger::Get().ReadRecords(NextLogSequence);
			bChanged = true;
		}
		if (Read.EvictedRecordCount > 0)
		{
			EvictedLogRecordCount += Read.EvictedRecordCount;
			bChanged = true;
		}
		bool bAddedLogs = false;
		for (FLogRecord& Log : Read.Records)
		{
			if (bCountUnread)
				UnreadImportantRecordCount = AccumulateConsoleUnreadImportantRecord(
					UnreadImportantRecordCount, Log.Level);
			State->Records.AddLog(std::move(Log));
			bAddedLogs = true;
			bChanged = true;
		}
		if (EvictedLogRecordCount > 0 && (Read.EvictedRecordCount > 0 || bAddedLogs))
		{
			// Keep the diagnostic visible after a full retained-history batch fills the model.
			State->Records.SetHistoryGap(EvictedLogRecordCount);
		}
		NextLogSequence = Read.NextSequence;
		if (bChanged) MarkRecordsChanged();
		return bChanged;
	}

	auto FConsolePanel::ApplyPendingRequests() -> bool
	{
		if (!State->bClearRequested.exchange(false, std::memory_order_acq_rel)) return false;
		State->Records.Clear();
		EvictedLogRecordCount = 0;
		bHasNewConsoleRecords = false;
		UnreadImportantRecordCount = 0;
		MarkRecordsChanged();
		return true;
	}

	auto FConsolePanel::MarkRecordsChanged() -> void
	{
		bVisibleRecordsDirty = true;
		bVisibleRecordLayoutDirty = true;
	}

	auto FConsolePanel::RefreshVisibleRecords() -> void
	{
		if (!bVisibleRecordsDirty) return;
		VisibleRecordIndices.clear();
		const std::deque<FConsoleRecord>& Records = State->Records.GetRecords();
		VisibleRecordIndices.reserve(Records.size());
		for (size_t Index = 0; Index < Records.size(); ++Index)
			if (IsRecordVisible(Index)) VisibleRecordIndices.push_back(Index);
		bVisibleRecordsDirty = false;
		bVisibleRecordLayoutDirty = true;
	}

	auto FConsolePanel::RefreshVisibleRecordLayout(float ContentWidth) -> bool
	{
		const float FontSize = ImGui::GetFontSize();
		if (!bVisibleRecordLayoutDirty
			&& std::abs(VisibleRecordLayoutWidth - ContentWidth) < 0.5f
			&& std::abs(VisibleRecordLayoutFontSize - FontSize) < 0.01f) return false;
		VisibleRecordOffsets.clear();
		VisibleRecordOffsets.reserve(VisibleRecordIndices.size() + 1);
		VisibleRecordOffsets.push_back(0.0f);
		const std::deque<FConsoleRecord>& Records = State->Records.GetRecords();
		for (const size_t RecordIndex : VisibleRecordIndices)
		{
			const float Height = MeasureConsoleRecordHeight(Records[RecordIndex], ContentWidth)
				+ ImGui::GetStyle().ItemSpacing.y;
			VisibleRecordOffsets.push_back(VisibleRecordOffsets.back() + Height);
		}
		VisibleRecordLayoutWidth = ContentWidth;
		VisibleRecordLayoutFontSize = FontSize;
		bVisibleRecordLayoutDirty = false;
		return true;
	}

	auto FConsolePanel::IsRecordVisible(size_t Index) const -> bool
	{
		const FConsoleRecord& Record = State->Records.GetRecords()[Index];
		if (Record.Type == EConsoleRecordType::Log)
		{
			const size_t LevelIndex = static_cast<size_t>(Record.Log.Level);
			if (LevelIndex >= LevelVisibility.size() || !LevelVisibility[LevelIndex]) return false;
			return SearchText[0] == '\0' || ContainsInsensitive(Record.Log.Module, SearchText.data()) ||
				ContainsInsensitive(Record.Log.CategoryOverride, SearchText.data()) || ContainsInsensitive(Record.Log.Message, SearchText.data());
		}
		return SearchText[0] == '\0' || ContainsInsensitive(Record.Text, SearchText.data());
	}

	auto FConsolePanel::CopyVisibleRecords() const -> void
	{
		std::string ClipboardText;
		const std::deque<FConsoleRecord>& Records = State->Records.GetRecords();
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			if (!IsRecordVisible(Index)) continue;
			const FConsoleRecord& Record = Records[Index];
			ClipboardText += Record.Type == EConsoleRecordType::Log ? FormatLog(Record.Log) : Record.Text;
			ClipboardText.push_back('\n');
		}
		ImGui::SetClipboardText(ClipboardText.c_str());
	}

	auto FConsolePanel::ExecuteCommand(std::string CommandLine) -> void
	{
		if (CommandLine.find_first_not_of(" \t\r\n") == std::string::npos) return;
		State->Records.AddText(EConsoleRecordType::Command, "> " + CommandLine);
		if (History.empty() || History.back() != CommandLine) History.push_back(CommandLine);
		if (History.size() > MaxCommandHistory) History.erase(History.begin());
		HistoryPosition = -1;
		const FConsoleCommandResult Result = FConsoleCommandRegistry::Get().Execute(CommandLine);
		(void)ApplyPendingRequests();
		if (!Result.Message.empty()) State->Records.AddText(Result.bSuccess ? EConsoleRecordType::Result : EConsoleRecordType::Error, Result.Message);
		MarkRecordsChanged();
		bHasNewConsoleRecords = true;
		ScrollToLatestFrames = 2;
	}

	auto FConsolePanel::Clear() -> void
	{
		State->bClearRequested.store(false, std::memory_order_release);
		State->Records.Clear();
		EvictedLogRecordCount = 0;
		MarkRecordsChanged();
		bHasNewConsoleRecords = false;
		UnreadImportantRecordCount = 0;
	}
} // namespace Durin::Editor::MainFrame
