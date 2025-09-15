#include "Logging/Logger.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"


auto FLogger::Get() -> FLogger&
{
	static FLogger instance;
	return instance;
}

auto FLogger::Log(ELogLevel Level, FStringView ModuleName, FStringView LogString, std::source_location SourceLocation) -> void
{
	spdlog::source_loc SpdSourceLocation = spdlog::source_loc{SourceLocation.file_name(), static_cast<int>(SourceLocation.line()), SourceLocation.function_name()};
	FString LogStringWithMoudule = std::format(STR("[{}] {}"), ModuleName, LogString);
	Logger->log(SpdSourceLocation, static_cast<spdlog::level::level_enum>(Level), LogStringWithMoudule);
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
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	console_sink->set_level(spdlog::level::debug);
	console_sink->set_pattern("[%H:%M:%S][%^%l%$]%v");

	auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Logs/Doge.log", true);
	file_sink->set_level(spdlog::level::trace);
	file_sink->set_pattern("[%Y-%m-%d %H:%M:%S][%^%l%$][%s:%#] %v");

	std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
	Logger = std::make_shared<spdlog::logger>("DogeLogger", sinks.begin(), sinks.end());

	spdlog::register_logger(Logger);
	Logger->set_level(spdlog::level::trace);
	spdlog::flush_every(std::chrono::seconds(5));
}

auto LoggerInit() -> void
{
	FLogger::Get();
}
