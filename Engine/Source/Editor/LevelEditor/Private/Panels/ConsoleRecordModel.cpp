#include "Panels/ConsoleRecordModel.h"

namespace Durin::Editor::Level
{
	auto FConsoleRecordModel::AddLog(FLogRecord Record) -> void
	{
		Records.push_back({EConsoleRecordType::Log, std::move(Record), {}});
		Trim();
	}

	auto FConsoleRecordModel::AddText(EConsoleRecordType Type, std::string Text) -> void
	{
		size_t LineStart = 0;
		for (;;)
		{
			const size_t LineEnd = Text.find('\n', LineStart);
			std::string Line = Text.substr(LineStart, LineEnd - LineStart);
			if (!Line.empty() && Line.back() == '\r') Line.pop_back();
			Records.push_back({Type, {}, std::move(Line)});
			Trim();
			if (LineEnd == std::string::npos || LineEnd + 1 == Text.size()) break;
			LineStart = LineEnd + 1;
		}
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
} // namespace Durin::Editor::Level
