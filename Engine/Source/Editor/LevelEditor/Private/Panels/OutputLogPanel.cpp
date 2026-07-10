#include "Panels/OutputLogPanel.h"

#include "Logging/Logger.h"
#include "MonaImGui.h"

namespace Durin
{
	struct FOutputLogState
	{
		std::mutex Mutex;
		std::vector<FLogRecord> PendingRecords;
		std::deque<FLogRecord> Records;
	};

	namespace
	{
		constexpr size_t MaxLogRecords = 5000;

		auto LevelIndex(ELogLevel Level) -> size_t
		{
			return static_cast<size_t>(Level);
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
			default: return "Unknown";
			}
		}

		auto LevelColor(ELogLevel Level) -> ImVec4
		{
			switch (Level)
			{
			case ELogLevel::Trace: return ImVec4(0.58f, 0.58f, 0.58f, 1.0f);
			case ELogLevel::Debug: return ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
			case ELogLevel::Info: return ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
			case ELogLevel::Warn: return ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
			case ELogLevel::Error: return ImVec4(1.0f, 0.32f, 0.32f, 1.0f);
			default: return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
			}
		}

		auto FormatRecord(const FLogRecord& Record) -> std::string
		{
			const std::time_t Time = std::chrono::system_clock::to_time_t(Record.Timestamp);
			std::tm LocalTime{};
			localtime_s(&LocalTime, &Time);
			std::array<char, 16> TimeText{};
			std::strftime(TimeText.data(), TimeText.size(), "%H:%M:%S", &LocalTime);
			return std::format("[{}][{}][{}] {}", TimeText.data(), LevelName(Record.Level), Record.Module, Record.Message);
		}

		auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool
		{
			if (Filter.empty())
			{
				return true;
			}
			std::string LowerText(Text);
			std::string LowerFilter(Filter);
			std::ranges::transform(LowerText, LowerText.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			std::ranges::transform(LowerFilter, LowerFilter.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return LowerText.find(LowerFilter) != std::string::npos;
		}
	} // namespace

	FOutputLogPanel::FOutputLogPanel()
		: State(std::make_shared<FOutputLogState>())
	{
		const std::weak_ptr<FOutputLogState> WeakState = State;
		ListenerHandle = FLogger::Get().AddListener([WeakState](const FLogRecord& Record) {
			if (const std::shared_ptr<FOutputLogState> SharedState = WeakState.lock())
			{
				std::scoped_lock Lock(SharedState->Mutex);
				SharedState->PendingRecords.push_back(Record);
			}
		});
	}

	FOutputLogPanel::~FOutputLogPanel()
	{
		FLogger::Get().RemoveListener(ListenerHandle);
		State.reset();
	}

	auto FOutputLogPanel::Draw(FLevelEditorContext& Context) -> void
	{
		(void)Context;
		const bool bReceivedNewRecords = DrainPendingRecords();
		if (!ImGui::Begin("Output Log###OutputLog", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

		if (ImGui::Button("Clear"))
		{
			std::scoped_lock Lock(State->Mutex);
			State->PendingRecords.clear();
			State->Records.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("Copy"))
		{
			std::string ClipboardText;
			for (const FLogRecord& Record : State->Records)
			{
				if (IsRecordVisible(Record))
				{
					ClipboardText += FormatRecord(Record);
					ClipboardText.push_back('\n');
				}
			}
			ImGui::SetClipboardText(ClipboardText.c_str());
		}
		ImGui::SameLine();
		ImGui::Checkbox("Auto-scroll", &bAutoScroll);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("###LogSearch", "Filter logs...", SearchText.data(), SearchText.size());

		for (size_t Index = 0; Index < LevelVisibility.size(); ++Index)
		{
			if (Index > 0)
			{
				ImGui::SameLine();
			}
			ImGui::Checkbox(LevelName(static_cast<ELogLevel>(Index)), &LevelVisibility[Index]);
		}
		ImGui::Separator();

		if (ImGui::BeginChild("LogRecords", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar))
		{
			ImGuiListClipper Clipper;
			std::vector<const FLogRecord*> VisibleRecords;
			VisibleRecords.reserve(State->Records.size());
			for (const FLogRecord& Record : State->Records)
			{
				if (IsRecordVisible(Record))
				{
					VisibleRecords.push_back(&Record);
				}
			}
			Clipper.Begin(static_cast<int>(VisibleRecords.size()));
			while (Clipper.Step())
			{
				for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
				{
					const FLogRecord& Record = *VisibleRecords[Index];
					ImGui::TextColored(LevelColor(Record.Level), "%s", FormatRecord(Record).c_str());
				}
			}
			if (bAutoScroll && bReceivedNewRecords)
			{
				ImGui::SetScrollHereY(1.0f);
			}
		}
		ImGui::EndChild();
		ImGui::End();
	}

	auto FOutputLogPanel::DrainPendingRecords() -> bool
	{
		std::vector<FLogRecord> PendingRecords;
		{
			std::scoped_lock Lock(State->Mutex);
			PendingRecords.swap(State->PendingRecords);
		}
		if (PendingRecords.empty())
		{
			return false;
		}
		for (FLogRecord& Record : PendingRecords)
		{
			State->Records.push_back(std::move(Record));
		}
		while (State->Records.size() > MaxLogRecords)
		{
			State->Records.pop_front();
		}
		return true;
	}

	auto FOutputLogPanel::IsRecordVisible(const FLogRecord& Record) const -> bool
	{
		const size_t Index = LevelIndex(Record.Level);
		if (Index >= LevelVisibility.size() || !LevelVisibility[Index])
		{
			return false;
		}
		if (SearchText[0] == '\0')
		{
			return true;
		}
		return ContainsInsensitive(Record.Module, SearchText.data()) || ContainsInsensitive(Record.Message, SearchText.data());
	}
} // namespace Durin
