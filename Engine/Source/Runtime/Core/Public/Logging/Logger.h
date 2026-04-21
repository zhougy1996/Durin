#pragma once

#include "CoreAPI.h"

namespace spdlog
{
	class logger;
}

namespace Doge
{
	enum class ELogLevel
	{
		Trace,
		Debug,
		Info,
		Warn,
		Error,
	};

	CORE_API auto StringToLogLevel(const std::string& InLogLevel) -> ELogLevel;

	class FLogger
	{
	public:
		FLogger();
		~FLogger();

		CORE_API static auto Get() -> FLogger&;

		template<typename... Args>
		void Log(ELogLevel Level, std::source_location Loc, std::string_view Module, std::format_string<Args...> Fmt, Args&&... args) {
			LogInternal(Level, Loc, Module, Fmt.get(), std::make_format_args(args...));
		}

		CORE_API auto SetConsoleLogLevel(ELogLevel Level);

		auto Initialize() -> void;

	private:
		CORE_API void LogInternal(ELogLevel Level, std::source_location Loc, std::string_view Module, std::string_view Fmt, std::format_args Args) const;
		class FImpl;

		std::unique_ptr<FImpl> Impl;

		ELogLevel ConsoleLogLevel = ELogLevel::Debug;

		bool bLogWithThreadName = false;
	};

	auto CORE_API LoggerInit() -> void;
}