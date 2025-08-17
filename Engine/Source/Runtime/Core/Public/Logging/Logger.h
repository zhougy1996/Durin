#pragma once

#include <source_location>
#include <string>
#include <memory>

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

class CORE_API FDogeLogger
{
public:
	static auto Get() -> FDogeLogger&;

	auto Log(ELogLevel Level, FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Trace(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Debug(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Info(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Warn(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

	static auto Error(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation = std::source_location::current()) -> void;

private:
	FDogeLogger();

	std::shared_ptr<spdlog::logger> Logger_;
};

auto CORE_API LoggerInit() -> void;

