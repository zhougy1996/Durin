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
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		static auto InputTextCallback(ImGuiInputTextCallbackData* Data) -> int;
		auto DrainPendingRecords() -> bool;
		auto IsRecordVisible(size_t Index) const -> bool;
		auto CopyVisibleRecords() const -> void;
		auto ExecuteCommand(std::string CommandLine) -> void;
		auto Clear() -> void;

		std::shared_ptr<FConsolePanelState> State;
		FLogListenerHandle ListenerHandle = 0;
		FConsoleCommandHandle ClearCommandHandle = 0;
		std::array<char, 128> SearchText{};
		std::array<char, 512> CommandText{};
		std::array<bool, 5> LevelVisibility{true, true, true, true, true};
		std::vector<std::string> History;
		int HistoryPosition = -1;
		bool bAutoScroll = true;
		bool bWasAtBottom = true;
		bool bHasNewConsoleRecords = false;
		bool bRefocusInput = false;
	};
} // namespace Durin
