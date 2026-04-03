#pragma once

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

	class CORE_API FLogger
	{
	public:
		static auto Get() -> FLogger&;

		auto Log(ELogLevel Level, std::string_view ModuleName, std::string_view LogString, std::source_location SourceLocation = std::source_location::current()) const -> void;

		static auto Trace(std::string_view ModuleName, std::string_view LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

		static auto Debug(std::string_view ModuleName, std::string_view LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

		static auto Info(std::string_view ModuleName, std::string_view LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

		static auto Warn(std::string_view ModuleName, std::string_view LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

		static auto Error(std::string_view ModuleName, std::string_view LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

		auto SetLogWithThreadName(bool bInLogWithThreadName) -> void { bLogWithThreadName = bInLogWithThreadName; }

	private:
		FLogger();

		std::shared_ptr<spdlog::logger> Logger;

		bool bLogWithThreadName = false;
	};

	auto CORE_API LoggerInit() -> void;
}