#include "Panels/ConsoleRecordModel.h"

namespace Durin
{
	auto FConsoleRecordModel::AddLog(FLogRecord Record) -> void
	{
		Records.push_back({EConsoleRecordType::Log, std::move(Record), {}});
		Trim();
	}

	auto FConsoleRecordModel::AddText(EConsoleRecordType Type, std::string Text) -> void
	{
		Records.push_back({Type, {}, std::move(Text)});
		Trim();
	}

	auto FConsoleRecordModel::SetHistoryGap(uint64 EvictedRecordCount) -> void
	{
		std::erase_if(Records, [](const FConsoleRecord& Record) { return Record.Type == EConsoleRecordType::HistoryGap; });
		if (EvictedRecordCount == 0) return;
		AddText(EConsoleRecordType::HistoryGap, std::format("Console skipped {} retained log records because logger history was evicted.", EvictedRecordCount));
	}

	auto FConsoleRecordModel::Clear() -> void
	{
		Records.clear();
	}

	auto FConsoleRecordModel::Trim() -> void
	{
		while (Records.size() > MaxRecords)
			Records.pop_front();
	}
} // namespace Durin
