#pragma once

#include "Console/ConsoleCommand.h"
#include "Logging/Logger.h"
#include "Panels/LevelEditorPanel.h"

struct ImGuiInputTextCallbackData;

namespace Durin
{
	struct FConsolePanelState;

	class FConsolePanel final : public ILevelEditorPanel
	{
	public:
		FConsolePanel();
		~FConsolePanel() override;

		auto GetWindowName() const -> const char* override { return "Console"; }
		auto TickWhenHidden() -> void override;
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		static auto InputTextCallback(ImGuiInputTextCallbackData* Data) -> int;
		auto PollLogRecords() -> bool;
		auto ApplyPendingRequests() -> bool;
		auto MarkRecordsChanged() -> void;
		auto RefreshVisibleRecords() -> void;
		auto IsRecordVisible(size_t Index) const -> bool;
		auto CopyVisibleRecords() const -> void;
		auto ExecuteCommand(std::string CommandLine) -> void;
		auto Clear() -> void;

		std::shared_ptr<FConsolePanelState> State;
		FConsoleCommandHandle ClearCommandHandle = 0;
		uint64 NextLogSequence = 1;
		uint64 EvictedLogRecordCount = 0;
		std::array<char, 128> SearchText{};
		std::array<char, 512> CommandText{};
		std::array<bool, static_cast<size_t>(ELogLevel::Fatal) + 1> LevelVisibility{true, true, true, true, true, true};
		std::vector<size_t> VisibleRecordIndices;
		std::vector<std::string> History;
		int HistoryPosition = -1;
		bool bAutoScroll = true;
		bool bHasNewConsoleRecords = false;
		bool bVisibleRecordsDirty = true;
		bool bRefocusInput = false;
	};
} // namespace Durin
