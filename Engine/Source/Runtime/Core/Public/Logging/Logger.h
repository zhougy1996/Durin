#pragma once

#include "CoreAPI.h"

namespace spdlog
{
	class logger;
}

namespace Durin
{
	enum class ELogLevel
	{
		Trace,
		Debug,
		Info,
		Warn,
		Error,
	};

	struct FLogRecord
	{
		std::chrono::system_clock::time_point Timestamp;
		ELogLevel Level = ELogLevel::Info;
		std::string Module;
		std::string Message;
	};

	using FLogListenerHandle = uint64;
	using FLogListener = std::function<void(const FLogRecord&)>;

	CORE_API auto StringToLogLevel(const std::string& InLogLevel) -> ELogLevel;

	class FLogger
	{
	public:
		FLogger();
		~FLogger();

		CORE_API static auto Get() -> FLogger&;

		template<typename... Args>
		void Log(ELogLevel Level, std::source_location Loc, std::string_view Module, std::format_string<Args...> Fmt, Args&&... args)
		{
			LogInternal(Level, Loc, Module, Fmt.get(), std::make_format_args(args...));
		}

		CORE_API auto SetConsoleLogLevel(ELogLevel Level);
		CORE_API auto AddListener(FLogListener Listener) -> FLogListenerHandle;
		CORE_API auto RemoveListener(FLogListenerHandle Handle) -> void;

		auto Initialize() -> void;

	private:
		CORE_API void LogInternal(ELogLevel Level, std::source_location Loc, std::string_view Module, std::string_view Fmt, std::format_args Args) const;
		class FImpl;

		std::unique_ptr<FImpl> Impl;

		ELogLevel ConsoleLogLevel = ELogLevel::Debug;
		mutable std::mutex ListenerMutex;
		std::unordered_map<FLogListenerHandle, FLogListener> Listeners;
		std::atomic<FLogListenerHandle> NextListenerHandle = 1;

		bool bLogWithThreadName = false;
	};

	auto CORE_API LoggerInit() -> void;
} // namespace Durin
