#include "Logging/Logger.h"

#include "Misc/AppConfigCache.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "Threading/RunnableThread.h"

namespace Doge
{
	class FLogger::FImpl
	{
		std::shared_ptr<spdlog::logger> SpdLogger;

		friend class FLogger;
	};

	auto FLogger::SetConsoleLogLevel(ELogLevel Level)
	{
		for (auto& Sink : Impl->SpdLogger->sinks())
		{
			if (auto ConsoleSink = std::dynamic_pointer_cast<spdlog::sinks::stdout_color_sink_mt>(Sink))
			{
				ConsoleSink->set_level(static_cast<spdlog::level::level_enum>(Level));
				break;
			}
		}
	}

	auto FLogger::Initialize() -> void
	{
		auto& SpdLogger = Impl->SpdLogger;
		std::filesystem::create_directories("Logs");

		const auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		ConsoleSink->set_level(spdlog::level::debug);
		ConsoleSink->set_pattern("[%H:%M:%S][%^%l%$]%v");

		const auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Logs/Doge.log", true);
		FileSink->set_level(spdlog::level::trace);
		FileSink->set_pattern("[%Y-%m-%d %H:%M:%S][%^%l%$][%s:%#] %v");

		std::vector<spdlog::sink_ptr> Sinks{ConsoleSink, FileSink};
		SpdLogger = std::make_shared<spdlog::logger>("DogeLogger", Sinks.begin(), Sinks.end());

		spdlog::register_logger(SpdLogger);
		SpdLogger->set_level(spdlog::level::trace);
		spdlog::flush_every(std::chrono::seconds(5));
	}

	FLogger::FLogger()
	{
		Impl = std::make_unique<FImpl>();
		// Create a simple console logger as default, will be replaced with a more complex one in LoggerInit
		Impl->SpdLogger = spdlog::stdout_color_mt("Default");
	}

	FLogger::~FLogger()
	{
	}

	auto FLogger::Get() -> FLogger&
	{
		static FLogger Instance;
		return Instance;
	}

	void FLogger::LogInternal(ELogLevel Level, std::source_location Loc, std::string_view Module, std::string_view Fmt, std::format_args Args) const
	{
		if (Impl->SpdLogger->should_log(static_cast<spdlog::level::level_enum>(Level)))
		{
			const auto SpdLoc = spdlog::source_loc{Loc.file_name(), static_cast<int>(Loc.line()), Loc.function_name()};
			std::string Message = std::format("[{}] {}", Module, std::vformat(Fmt, Args));
			Impl->SpdLogger->log(SpdLoc, static_cast<spdlog::level::level_enum>(Level), Message);
		}
	}

	auto StringToLogLevel(const std::string& InLogLevel) -> ELogLevel
	{
		static const std::unordered_map<std::string_view, ELogLevel> LevelMap = {
			{"Trace", ELogLevel::Trace},
			{"Debug", ELogLevel::Debug},
			{"Info",  ELogLevel::Info},
			{"Warn",  ELogLevel::Warn},
			{"Error", ELogLevel::Error}
		};

		if (auto it = LevelMap.find(InLogLevel); it != LevelMap.end())
		{
			return it->second;
		}

		DOGE_WARN("Invalid log level in config: {}, defaulting to Debug", InLogLevel);
		return ELogLevel::Debug;
	}

	auto LoggerInit() -> void
	{
		check(IsAppConfigLoaded());
		FLogger& Logger = FLogger::Get();
		Logger.Initialize();
		Logger.SetConsoleLogLevel(StringToLogLevel(GAppConfig.GetStringValue("LogLevel")));
	}
}