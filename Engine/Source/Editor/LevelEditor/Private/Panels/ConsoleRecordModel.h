#pragma once

#include "Logging/Logger.h"

namespace Durin
{
	enum class EConsoleRecordType
	{
		Log,
		Command,
		Result,
		HistoryGap,
		Error
	};

	struct FConsoleRecord
	{
		EConsoleRecordType Type = EConsoleRecordType::Log;
		FLogRecord Log;
		std::string Text;
	};

	class FConsoleRecordModel
	{
	public:
		static constexpr size_t MaxRecords = 5000;
		static constexpr size_t LogLevelCount = static_cast<size_t>(ELogLevel::Fatal) + 1;

		auto AddLog(FLogRecord Record) -> void;
		auto AddText(EConsoleRecordType Type, std::string Text) -> void;
		auto SetHistoryGap(uint64 EvictedRecordCount) -> void;
		auto Clear() -> void;
		auto GetRecords() const -> const std::deque<FConsoleRecord>& { return Records; }

	private:
		auto Trim() -> void;

		std::deque<FConsoleRecord> Records;
	};
} // namespace Durin
