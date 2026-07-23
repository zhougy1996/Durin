#pragma once

#include "CoreAPI.h"

namespace Durin
{
	enum class ELogLevel
	{
		Trace,
		Debug,
		Info,
		Warn,
		Error,
		Fatal,
	};

	struct FLogRecord
	{
		std::chrono::system_clock::time_point Timestamp;
		ELogLevel Level = ELogLevel::Info;
		std::string Module;
		std::string Message;

		uint64 Sequence = 0;
		uint32 ThreadId = 0;
		std::string ThreadName;
		std::string File;
		uint32 Line = 0;
		std::string Function;
	};

	struct FLogSettings
	{
		ELogLevel ConsoleLevel = ELogLevel::Debug;
		ELogLevel FileLevel = ELogLevel::Trace;
		uint32 QueueCapacity = 8192;
		uint32 HistoryCapacity = 5000;
		uint32 FlushIntervalMilliseconds = 1000;
		uint64 MaxFileSizeBytes = 20ull * 1024ull * 1024ull;
		uint32 MaxFilesPerSession = 5;
		uint32 MaxSessions = 10;
		std::string LogDirectory;
		std::string ProfileName = "Durin";
	};

	struct FLogReadResult
	{
		std::vector<FLogRecord> Records;
		uint64 OldestAvailableSequence = 0;
		uint64 NewestAvailableSequence = 0;
		uint64 NextSequence = 1;
		uint64 EvictedRecordCount = 0;
	};

	CORE_API auto StringToLogLevel(std::string_view InLogLevel, ELogLevel DefaultLevel = ELogLevel::Debug) -> ELogLevel;

	class FLogger
	{
	public:
		CORE_API FLogger();
		CORE_API ~FLogger();

		FLogger(const FLogger&) = delete;
		auto operator=(const FLogger&) -> FLogger& = delete;

		CORE_API static auto Get() -> FLogger&;

		// Error and Fatal calls wait for active sink attempts and flushing.
		// Shutdown may release an accepted reliable call early.
		template<typename... Args>
		void Log(ELogLevel Level, std::source_location Loc, std::string_view Module, std::format_string<Args...> Fmt, Args&&... args)
		{
			if (!ShouldLog(Level))
			{
				return;
			}
			LogInternal(Level, Loc, Module, Fmt.get(), std::make_format_args(args...));
		}

		CORE_API auto ShouldLog(ELogLevel Level) const -> bool;
		CORE_API auto SetConsoleLogLevel(ELogLevel Level) -> void;
		// NextSequence identifies the next desired record. Results are ordered and
		// report when that cursor has fallen behind the retained history window.
		CORE_API auto ReadRecords(uint64 NextSequence, uint32 MaxRecords = 512) const -> FLogReadResult;

		// Compatibility entry point using the standard runtime defaults.
		CORE_API auto Initialize() -> void;
		CORE_API auto Initialize(const FLogSettings& Settings) -> bool;
		CORE_API auto Flush() -> void;
		CORE_API auto Shutdown() -> void;

	private:
		CORE_API auto LogInternal(ELogLevel Level, std::source_location Loc, std::string_view Module, std::string_view Fmt, std::format_args Args) const -> void;

		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	CORE_API auto LoggerInit() -> void;
	CORE_API auto LoggerShutdown() -> void;
} // namespace Durin
