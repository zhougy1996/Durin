#pragma once

#include "Definitions.Core.h"

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

//class CORE_API FDogeLogger
//{
//public:
//	static auto Get() -> FDogeLogger&;
//
//	auto Log(ELogLevel Level, const char* ModuleName, const std::string& LogString, std::source_location SourceLocation = std::source_location::current()) -> void;
//
//	static auto Trace(const char* ModuleName, const std::string& LogString, std::source_location SourceLocation = std::source_location::current()) -> void;
//
//	static auto Debug(const char* ModuleName, const std::string& LogString, std::source_location SourceLocation = std::source_location::current()) -> void;
//
//	static auto Info(const char* ModuleName, const std::string& LogString, std::source_location SourceLocation = std::source_location::current()) -> void;
//
//	static auto Warn(const char* ModuleName, const std::string& LogString, std::source_location SourceLocation = std::source_location::current()) -> void;
//
//	static auto Error(const char* ModuleName, const std::string& LogString, std::source_location SourceLocation = std::source_location::current()) -> void;
//
//private:
//	FDogeLogger();
//
//	std::shared_ptr<spdlog::logger> Logger_;
//};

auto CORE_API LoggerInit() -> void;
