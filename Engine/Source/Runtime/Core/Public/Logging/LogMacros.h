#pragma once

#include "Logging/Logger.h"

#define DOGE_DEBUG(...) FLogger::Debug(MODULE_NAME, std::format(__VA_ARGS__))
#define DOGE_INFO(...) FLogger::Info(MODULE_NAME, std::format(__VA_ARGS__))
#define DOGE_WARN(...) FLogger::Warn(MODULE_NAME, std::format(__VA_ARGS__))
#define DOGE_ERROR(...) FLogger::Error(MODULE_NAME, std::format(__VA_ARGS__))
