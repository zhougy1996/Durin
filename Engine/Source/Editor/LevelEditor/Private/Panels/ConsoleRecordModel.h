#pragma once

#include "Logging/Logger.h"

namespace Durin::Editor::Level
{
	// Distinguishes log output from command history and command results.
	enum class EConsoleRecordType
	{
		Log,
		Command,
		Result,
		HistoryGap,
		Error
	};

	// Stores one timestamped, searchable console entry.
	struct FConsoleRecord
	{
		EConsoleRecordType Type = EConsoleRecordType::Log;
		FLogRecord Log;
		std::string Text;
	};

	// Builds a bounded filtered view over editor console history.
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
} // namespace Durin::Editor::Level
