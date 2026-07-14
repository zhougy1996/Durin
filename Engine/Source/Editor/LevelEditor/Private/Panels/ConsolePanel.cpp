#include "Panels/ConsolePanel.h"

#include "Icons/FontAwesomeIcons.h"
#include "LevelEditorHelpers.h"
#include "Logging/Logger.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin
{
	enum class EConsoleRecordType
	{
		Log,
		Command,
		Result,
		Error
	};

	struct FConsoleRecord
	{
		EConsoleRecordType Type = EConsoleRecordType::Log;
		FLogRecord Log;
		std::string Text;
	};

	struct FConsolePanelState
	{
		std::mutex Mutex;
		std::vector<FLogRecord> PendingLogs;
		std::deque<FConsoleRecord> Records;
	};

	namespace
	{
		using LevelEditorHelpers::DrawToolbarIconButton;
		using StringUtils::ContainsInsensitive;

		constexpr size_t MaxConsoleRecords = 5000;
		constexpr size_t MaxCommandHistory = 100;

		auto LevelName(ELogLevel Level) -> const char*
		{
			switch (Level)
			{
			case ELogLevel::Trace: return "Trace";
			case ELogLevel::Debug: return "Debug";
			case ELogLevel::Info: return "Info";
			case ELogLevel::Warn: return "Warn";
			case ELogLevel::Error: return "Error";
			default: return "Unknown";
			}
		}

		auto LevelColor(ELogLevel Level) -> ImVec4
		{
			switch (Level)
			{
			case ELogLevel::Trace: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
			case ELogLevel::Debug: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info);
			case ELogLevel::Info: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
			case ELogLevel::Warn: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning);
			case ELogLevel::Error: return MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error);
			default: return {1.0f, 1.0f, 1.0f, 1.0f};
			}
		}

		auto FormatTime(const FLogRecord& Record) -> std::array<char, 16>
		{
			const std::time_t Time = std::chrono::system_clock::to_time_t(Record.Timestamp);
			std::tm LocalTime{};
			localtime_s(&LocalTime, &Time);
			std::array<char, 16> TimeText{};
			std::strftime(TimeText.data(), TimeText.size(), "%H:%M:%S", &LocalTime);
			return TimeText;
		}

		auto FormatLog(const FLogRecord& Record) -> std::string
		{
			const std::array<char, 16> TimeText = FormatTime(Record);
			return std::format("[{}][{}][{}] {}", TimeText.data(), LevelName(Record.Level), Record.Module, Record.Message);
		}

		auto DrawLogRecord(const FLogRecord& Record) -> void
		{
			const std::array<char, 16> TimeText = FormatTime(Record);
			ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::ConsoleTimestamp), "%s", TimeText.data());
			ImGui::SameLine(0.0f, MonaImGui::GetUIStyleMetrics().SpacingM);
			ImGui::TextColored(LevelColor(Record.Level), "%-5s", LevelName(Record.Level));
			ImGui::SameLine(0.0f, MonaImGui::GetUIStyleMetrics().SpacingM);
			ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::ConsoleModule), "[%s]", Record.Module.c_str());
			ImGui::SameLine(0.0f, MonaImGui::GetUIStyleMetrics().SpacingM);
			ImGui::TextUnformatted(Record.Message.c_str());
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

	FConsolePanel::FConsolePanel()
		: State(std::make_shared<FConsolePanelState>())
	{
		const std::weak_ptr<FConsolePanelState> WeakState = State;
		ListenerHandle = FLogger::Get().AddListener([WeakState](const FLogRecord& Record) {
			if (const auto SharedState = WeakState.lock())
			{
				std::scoped_lock Lock(SharedState->Mutex);
				SharedState->PendingLogs.push_back(Record);
			}
		});
		ClearCommandHandle = FConsoleCommandRegistry::Get().RegisterCommand({"clear", "Clears the editor console.", "clear", [WeakState](std::span<const std::string> Args) {
																				 if (!Args.empty()) return FConsoleCommandResult::Failure("Usage: clear");
																				 if (const auto SharedState = WeakState.lock())
																				 {
																					 std::scoped_lock Lock(SharedState->Mutex);
																					 SharedState->PendingLogs.clear();
																					 SharedState->Records.clear();
																				 }
																				 return FConsoleCommandResult::Success();
																			 }});
	}

	FConsolePanel::~FConsolePanel()
	{
		FConsoleCommandRegistry::Get().UnregisterCommand(ClearCommandHandle);
		FLogger::Get().RemoveListener(ListenerHandle);
		State.reset();
	}

	auto FConsolePanel::Draw(FLevelEditorContext& Context) -> void
	{
		(void)Context;
		const bool bReceivedRecords = DrainPendingRecords() || std::exchange(bHasNewConsoleRecords, false);
		if (!ImGui::Begin("Console###OutputLog", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

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
					ImGui::MenuItem(LevelName(Level), nullptr, &LevelVisibility[Index]);
					ImGui::PopStyleColor();
				}
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("###ConsoleSearch", "Search output...", SearchText.data(), SearchText.size());
		ImGui::Separator();

		const float InputHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		if (ImGui::BeginChild("ConsoleRecords", ImVec2(0, -InputHeight), false, ImGuiWindowFlags_HorizontalScrollbar))
		{
			std::vector<size_t> VisibleRecords;
			for (size_t Index = 0; Index < State->Records.size(); ++Index)
				if (IsRecordVisible(Index)) VisibleRecords.push_back(Index);
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(VisibleRecords.size()));
			while (Clipper.Step())
			{
				for (int VisibleIndex = Clipper.DisplayStart; VisibleIndex < Clipper.DisplayEnd; ++VisibleIndex)
				{
					const FConsoleRecord& Record = State->Records[VisibleRecords[VisibleIndex]];
					if (Record.Type == EConsoleRecordType::Log)
						DrawLogRecord(Record.Log);
					else
					{
						const ImVec4 Color = Record.Type == EConsoleRecordType::Command ? MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info) :
											 Record.Type == EConsoleRecordType::Error	? MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error) :
																						  ImGui::GetStyleColorVec4(ImGuiCol_Text);
						ImGui::TextColored(Color, "%s", Record.Text.c_str());
					}
				}
			}
			if (bAutoScroll && bWasAtBottom && bReceivedRecords) ImGui::SetScrollHereY(1.0f);
			bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
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
		if (ImGui::InputTextWithHint("###ConsoleInput", "Type a command (Tab complete, Up/Down history)", CommandText.data(), CommandText.size(), Flags, InputTextCallback, this))
		{
			ExecuteCommand(CommandText.data());
			CommandText.fill('\0');
			bRefocusInput = true;
		}
		ImGui::End();
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
				Panel->State->Records.push_back({EConsoleRecordType::Result, {}, std::move(Text)});
				Panel->bHasNewConsoleRecords = true;
			}
		}
		return 0;
	}

	auto FConsolePanel::DrainPendingRecords() -> bool
	{
		std::vector<FLogRecord> PendingLogs;
		{
			std::scoped_lock Lock(State->Mutex);
			PendingLogs.swap(State->PendingLogs);
		}
		for (FLogRecord& Log : PendingLogs)
			State->Records.push_back({EConsoleRecordType::Log, std::move(Log), {}});
		while (State->Records.size() > MaxConsoleRecords)
			State->Records.pop_front();
		return !PendingLogs.empty();
	}

	auto FConsolePanel::IsRecordVisible(size_t Index) const -> bool
	{
		const FConsoleRecord& Record = State->Records[Index];
		if (Record.Type == EConsoleRecordType::Log)
		{
			const size_t LevelIndex = static_cast<size_t>(Record.Log.Level);
			if (LevelIndex >= LevelVisibility.size() || !LevelVisibility[LevelIndex]) return false;
			return SearchText[0] == '\0' || ContainsInsensitive(Record.Log.Module, SearchText.data()) || ContainsInsensitive(Record.Log.Message, SearchText.data());
		}
		return SearchText[0] == '\0' || ContainsInsensitive(Record.Text, SearchText.data());
	}

	auto FConsolePanel::CopyVisibleRecords() const -> void
	{
		std::string ClipboardText;
		for (size_t Index = 0; Index < State->Records.size(); ++Index)
		{
			if (!IsRecordVisible(Index)) continue;
			const FConsoleRecord& Record = State->Records[Index];
			ClipboardText += Record.Type == EConsoleRecordType::Log ? FormatLog(Record.Log) : Record.Text;
			ClipboardText.push_back('\n');
		}
		ImGui::SetClipboardText(ClipboardText.c_str());
	}

	auto FConsolePanel::ExecuteCommand(std::string CommandLine) -> void
	{
		if (CommandLine.find_first_not_of(" \t\r\n") == std::string::npos) return;
		State->Records.push_back({EConsoleRecordType::Command, {}, "> " + CommandLine});
		if (History.empty() || History.back() != CommandLine) History.push_back(CommandLine);
		if (History.size() > MaxCommandHistory) History.erase(History.begin());
		HistoryPosition = -1;
		const FConsoleCommandResult Result = FConsoleCommandRegistry::Get().Execute(CommandLine);
		if (!Result.Message.empty()) State->Records.push_back({Result.bSuccess ? EConsoleRecordType::Result : EConsoleRecordType::Error, {}, Result.Message});
		if (!Result.bSuccess) DURIN_ERROR("Console command failed: {}", Result.Message);
		while (State->Records.size() > MaxConsoleRecords)
			State->Records.pop_front();
		bHasNewConsoleRecords = true;
	}

	auto FConsolePanel::Clear() -> void
	{
		std::scoped_lock Lock(State->Mutex);
		State->PendingLogs.clear();
		State->Records.clear();
		bWasAtBottom = true;
		bHasNewConsoleRecords = false;
	}
} // namespace Durin
