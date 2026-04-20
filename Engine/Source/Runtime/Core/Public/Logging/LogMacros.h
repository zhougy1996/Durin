#pragma once

#include "Logging/Logger.h"

#define DOGE_LOG(LogLevel, ...) FLogger::Get().Log(LogLevel, std::source_location::current(), MODULE_NAME, __VA_ARGS__)

#define DOGE_TRACE(...) DOGE_LOG(ELogLevel::Trace, __VA_ARGS__)
#define DOGE_DEBUG(...) DOGE_LOG(ELogLevel::Debug, __VA_ARGS__)
#define DOGE_INFO(...) DOGE_LOG(ELogLevel::Info, __VA_ARGS__)
#define DOGE_WARN(...) DOGE_LOG(ELogLevel::Warn, __VA_ARGS__)
#define DOGE_ERROR(...) DOGE_LOG(ELogLevel::Error, __VA_ARGS__)
