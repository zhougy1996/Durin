#pragma once

#include "Console/ConsoleCommand.h"
#include "Logging/Logger.h"

struct ImGuiInputTextCallbackData;

namespace Durin::Editor::MainFrame
{
	struct FConsolePanelState;

	// Draws console history and submits commands through the editor console.
	class FConsolePanel final
	{
	public:
		explicit FConsolePanel(FModuleOwnedCallbackGate OwnerGate = {});
		~FConsolePanel();

		auto Tick() -> void;
		auto DrawContents() -> void;
		auto RequestInputFocus() -> void { bRefocusInput = true; }
		auto RequestScrollToLatest() -> void { ScrollToLatestFrames = 2; }
		auto GetUnreadImportantRecordCount() const -> uint32
		{
			return UnreadImportantRecordCount;
		}

	private:
		static auto InputTextCallback(ImGuiInputTextCallbackData* Data) -> int;
		auto PollLogRecords(bool bCountUnread = false) -> bool;
		auto DrawBody(bool bReceivedRecords) -> void;
		auto ApplyPendingRequests() -> bool;
		auto MarkRecordsChanged() -> void;
		auto RefreshVisibleRecords() -> void;
		auto RefreshVisibleRecordLayout(float ContentWidth) -> bool;
		auto IsRecordVisible(size_t Index) const -> bool;
		auto CopyVisibleRecords() const -> void;
		auto ExecuteCommand(std::string CommandLine) -> void;
		auto Clear() -> void;

		std::shared_ptr<FConsolePanelState> State;
		FConsoleCommandHandle ClearCommandHandle = 0;
		FConsoleCommandHandle AssetFixUpCommandHandle = 0;
		uint64 NextLogSequence = 1;
		uint64 EvictedLogRecordCount = 0;
		std::array<char, 128> SearchText{};
		std::array<char, 512> CommandText{};
		std::array<bool, static_cast<size_t>(ELogLevel::Fatal) + 1> LevelVisibility{true, true, true, true, true, true};
		std::vector<size_t> VisibleRecordIndices;
		std::vector<float> VisibleRecordOffsets;
		std::vector<std::string> History;
		int HistoryPosition = -1;
		bool bAutoScroll = true;
		bool bHasNewConsoleRecords = false;
		bool bVisibleRecordsDirty = true;
		bool bVisibleRecordLayoutDirty = true;
		bool bRefocusInput = false;
		float VisibleRecordLayoutWidth = 0.0f;
		float VisibleRecordLayoutFontSize = 0.0f;
		uint8 ScrollToLatestFrames = 0;
		uint32 UnreadImportantRecordCount = 0;
	};
} // namespace Durin::Editor::MainFrame
