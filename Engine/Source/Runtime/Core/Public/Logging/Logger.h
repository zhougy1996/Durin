#pragma once

namespace spdlog
{
class logger;
}

enum class CORE_API ELogLevel
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

	auto Log(ELogLevel Level, FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) const -> void;

	static auto Trace(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Debug(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Info(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Warn(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Error(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

private:
	FLogger();

	std::shared_ptr<spdlog::logger> Logger;
};

auto CORE_API LoggerInit() -> void;

