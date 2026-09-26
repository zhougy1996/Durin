#pragma once

#include "Logging/Logger.h"
#include "NativeTestSupport.h"

// Observe the existing logger instead of making business results carry test diagnostics.
class FCacheLogCapture
{
public:
	FCacheLogCapture()
	{
		if (ActiveCaptures == 0)
		{
			Durin::FLogSettings Settings;
			Settings.LogDirectory = (Durin::Testing::GetTestWorkDirectory() / "CacheDiagnosticsLogs").generic_string();
			if (!Durin::FLogger::Get().Initialize(Settings)) throw std::runtime_error("Could not initialize cache test logging");
		}
		++ActiveCaptures;
		Reset();
	}
	~FCacheLogCapture() { if (--ActiveCaptures == 0) Durin::FLogger::Get().Shutdown(); }
	FCacheLogCapture(const FCacheLogCapture&) = delete;
	auto operator=(const FCacheLogCapture&) -> FCacheLogCapture& = delete;
	auto Reset() -> void
	{
		Durin::FLogger::Get().Flush();
		Cursor = Durin::FLogger::Get().ReadRecords(0, 1).NewestAvailableSequence + 1;
	}
	auto Records() const -> std::vector<Durin::FLogRecord>
	{
		Durin::FLogger::Get().Flush();
		auto Records = Durin::FLogger::Get().ReadRecords(Cursor, 5000).Records;
		std::erase_if(Records, [](const auto& Record) { return Record.GetCategory() != "DerivedData"; });
		return Records;
	}
	auto Has(std::string_view Operation) const -> bool
	{
		return std::ranges::any_of(Records(), [&](const auto& Record) {
			return Record.Message.find(std::string("cache ") + std::string(Operation) + " [") != std::string::npos;
		});
	}
	auto empty() const -> bool { return Records().empty(); }
	auto size() const -> size_t { return Records().size(); }
	auto front() const -> Durin::FLogRecord { return Records().front(); }
private:
	inline static uint32 ActiveCaptures = 0;
	uint64 Cursor = 0;
};
