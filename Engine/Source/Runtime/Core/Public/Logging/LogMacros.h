#pragma once

#include "Logging/Logger.h"

#define DURIN_LOG(LogLevel, ...) FLogger::Get().Log(LogLevel, std::source_location::current(), MODULE_NAME, __VA_ARGS__)
#define DURIN_LOG_CATEGORY(CategoryName, LogLevel, ...) FLogger::Get().LogCategory(LogLevel, std::source_location::current(), MODULE_NAME, CategoryName, __VA_ARGS__)

#if DURIN_BUILD_SHIPPING
	#define DURIN_TRACE(...) ((void)0)
	#define DURIN_DEBUG(...) ((void)0)
	#define DURIN_TRACE_CATEGORY(...) ((void)0)
	#define DURIN_DEBUG_CATEGORY(...) ((void)0)
#else
	#define DURIN_TRACE(...) DURIN_LOG(ELogLevel::Trace, __VA_ARGS__)
	#define DURIN_DEBUG(...) DURIN_LOG(ELogLevel::Debug, __VA_ARGS__)
	#define DURIN_TRACE_CATEGORY(CategoryName, ...) DURIN_LOG_CATEGORY(CategoryName, ELogLevel::Trace, __VA_ARGS__)
	#define DURIN_DEBUG_CATEGORY(CategoryName, ...) DURIN_LOG_CATEGORY(CategoryName, ELogLevel::Debug, __VA_ARGS__)
#endif
#define DURIN_INFO(...) DURIN_LOG(ELogLevel::Info, __VA_ARGS__)
#define DURIN_WARN(...) DURIN_LOG(ELogLevel::Warn, __VA_ARGS__)
#define DURIN_ERROR(...) DURIN_LOG(ELogLevel::Error, __VA_ARGS__)
#define DURIN_FATAL(...) DURIN_LOG(ELogLevel::Fatal, __VA_ARGS__)
#define DURIN_INFO_CATEGORY(CategoryName, ...) DURIN_LOG_CATEGORY(CategoryName, ELogLevel::Info, __VA_ARGS__)
#define DURIN_WARN_CATEGORY(CategoryName, ...) DURIN_LOG_CATEGORY(CategoryName, ELogLevel::Warn, __VA_ARGS__)
#define DURIN_ERROR_CATEGORY(CategoryName, ...) DURIN_LOG_CATEGORY(CategoryName, ELogLevel::Error, __VA_ARGS__)
#define DURIN_FATAL_CATEGORY(CategoryName, ...) DURIN_LOG_CATEGORY(CategoryName, ELogLevel::Fatal, __VA_ARGS__)
