#include "Logging/Logger.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "Threading/RunnableThread.h"

namespace Doge
{

	auto FLogger::Get() -> FLogger&
	{
		static FLogger instance;
		return instance;
	}

	auto FLogger::Log(ELogLevel Level, FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) const -> void
	{
		const auto SpdSourceLocation = spdlog::source_loc{SourceLocation.file_name(), static_cast<int>(SourceLocation.line()), SourceLocation.function_name()};
		if (bLogWithThreadName)
		{
			Logger->log(SpdSourceLocation, static_cast<spdlog::level::level_enum>(Level), "[{}][{}] {}", GetCurrentThreadName(), ModuleName, LogString);
		}
		else
		{
			Logger->log(SpdSourceLocation, static_cast<spdlog::level::level_enum>(Level), "[{}] {}", ModuleName, LogString);
		}
	}

	auto FLogger::Trace(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) -> void
	{
		Get().Log(ELogLevel::Trace, ModuleName, LogString, SourceLocation);
	}

	auto FLogger::Debug(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) -> void
	{
		Get().Log(ELogLevel::Debug, ModuleName, LogString, SourceLocation);
	}

	auto FLogger::Info(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) -> void
	{
		Get().Log(ELogLevel::Info, ModuleName, LogString, SourceLocation);
	}

	auto FLogger::Warn(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) -> void
	{
		Get().Log(ELogLevel::Warn, ModuleName, LogString, SourceLocation);
	}

	auto FLogger::Error(FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) -> void
	{
		Get().Log(ELogLevel::Error, ModuleName, LogString, SourceLocation);
	}

	FLogger::FLogger()
	{
		const auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	#if DOGE_BUILD_DEBUG
		ConsoleSink->set_level(spdlog::level::debug);
	#else
		ConsoleSink->set_level(spdlog::level::info);
	#endif
		ConsoleSink->set_pattern("[%H:%M:%S][%^%l%$]%v");

		const auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Logs/Doge.log", true);
		FileSink->set_level(spdlog::level::trace);
		FileSink->set_pattern("[%Y-%m-%d %H:%M:%S][%^%l%$][%s:%#] %v");

		std::vector<spdlog::sink_ptr> Sinks{ConsoleSink, FileSink};
		Logger = std::make_shared<spdlog::logger>("DogeLogger", Sinks.begin(), Sinks.end());

		spdlog::register_logger(Logger);
		Logger->set_level(spdlog::level::trace);
		spdlog::flush_every(std::chrono::seconds(5));
	}

	auto LoggerInit() -> void
	{
		FLogger& Logger = FLogger::Get();
		Logger.SetLogWithThreadName(false);
	}
}