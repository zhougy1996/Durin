#pragma once

#include "Logging/Logger.h"

#define DURIN_LOG(LogLevel, ...) FLogger::Get().Log(LogLevel, std::source_location::current(), MODULE_NAME, __VA_ARGS__)

#define DURIN_TRACE(...) DURIN_LOG(ELogLevel::Trace, __VA_ARGS__)
#define DURIN_DEBUG(...) DURIN_LOG(ELogLevel::Debug, __VA_ARGS__)
#define DURIN_INFO(...) DURIN_LOG(ELogLevel::Info, __VA_ARGS__)
#define DURIN_WARN(...) DURIN_LOG(ELogLevel::Warn, __VA_ARGS__)
#define DURIN_ERROR(...) DURIN_LOG(ELogLevel::Error, __VA_ARGS__)
