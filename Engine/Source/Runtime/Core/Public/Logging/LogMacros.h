#pragma once

#include <format>
#include "Logging/Logger.h"

#define DOGE_DEBUG(...) FDogeLogger::Debug(MODULE_NAME, std::format(__VA_ARGS__))
#define DOGE_INFO(...) FDogeLogger::Info(MODULE_NAME, std::format(__VA_ARGS__))
#define DOGE_WARN(...) FDogeLogger::Warn(MODULE_NAME, std::format(__VA_ARGS__))
#define DOGE_ERROR(...) FDogeLogger::Error(MODULE_NAME, std::format(__VA_ARGS__))
