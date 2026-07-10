#pragma once

#include "Logging/Logger.h"
#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	struct FOutputLogState;

	class FOutputLogPanel final : public ILevelEditorPanel
	{
	public:
		FOutputLogPanel();
		~FOutputLogPanel() override;

		auto GetWindowName() const -> const char* override { return "Output Log"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		auto DrainPendingRecords() -> bool;
		auto IsRecordVisible(const FLogRecord& Record) const -> bool;

		std::shared_ptr<FOutputLogState> State;
		FLogListenerHandle ListenerHandle = 0;
		std::array<char, 128> SearchText{};
		std::array<bool, 5> LevelVisibility{true, true, true, true, true};
		bool bAutoScroll = true;
	};
} // namespace Durin
