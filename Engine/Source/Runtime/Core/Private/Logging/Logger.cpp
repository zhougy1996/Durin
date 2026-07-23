#include "Logging/Logger.h"

#include "CoreGlobals.h"
#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AppConfig.h"
#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>

namespace Durin
{
	namespace
	{
		constexpr uint32 DefaultHistoryCapacity = 5000;
		constexpr uint32 MinHistoryCapacity = 256;
		constexpr uint32 MaxHistoryCapacity = 65536;
		constexpr uint32 MinReadBatchSize = 1;
		constexpr uint32 MaxReadBatchSize = 4096;

		auto ToSpdLevel(ELogLevel Level) -> spdlog::level::level_enum
		{
			return static_cast<spdlog::level::level_enum>(Level);
		}

		auto LevelName(ELogLevel Level) -> const char*
		{
			switch (Level)
			{
			case ELogLevel::Trace: return "Trace";
			case ELogLevel::Debug: return "Debug";
			case ELogLevel::Info: return "Info";
			case ELogLevel::Warn: return "Warn";
			case ELogLevel::Error: return "Error";
			case ELogLevel::Fatal: return "Fatal";
			default: return "Unknown";
			}
		}

		auto TryParseLogLevel(std::string_view Text, ELogLevel& OutLevel) -> bool
		{
			std::string Normalized(Text);
			std::ranges::transform(Normalized, Normalized.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			static const std::unordered_map<std::string_view, ELogLevel> Levels = {
				{"trace", ELogLevel::Trace}, {"debug", ELogLevel::Debug}, {"info", ELogLevel::Info},
				{"warn", ELogLevel::Warn}, {"warning", ELogLevel::Warn}, {"error", ELogLevel::Error},
				{"fatal", ELogLevel::Fatal}, {"critical", ELogLevel::Fatal},
			};
			if (const auto Iterator = Levels.find(Normalized); Iterator != Levels.end())
			{
				OutLevel = Iterator->second;
				return true;
			}
			return false;
		}

		auto SanitizeFileComponent(std::string Value) -> std::string
		{
			for (char& Character : Value)
			{
				if (!std::isalnum(static_cast<unsigned char>(Character)) && Character != '-' && Character != '_') Character = '_';
			}
			return Value.empty() ? "Durin" : Value;
		}

		auto SessionTimestamp() -> std::string
		{
			const std::time_t Time = std::time(nullptr);
			std::tm LocalTime{};
#ifdef _WIN32
			localtime_s(&LocalTime, &Time);
#else
			localtime_r(&Time, &LocalTime);
#endif
			std::array<char, 32> Buffer{};
			std::strftime(Buffer.data(), Buffer.size(), "%Y%m%d-%H%M%S", &LocalTime);
			return Buffer.data();
		}

		auto IsRotationStem(std::string_view Stem) -> bool
		{
			const size_t Dot = Stem.find_last_of('.');
			return Dot != std::string_view::npos && Dot + 1 < Stem.size() &&
				std::ranges::all_of(Stem.substr(Dot + 1), [](unsigned char Character) { return std::isdigit(Character); });
		}

		auto CleanupOldSessions(const std::filesystem::path& Directory, std::string_view Prefix, uint32 MaxSessions) -> void
		{
			struct FSessionFile
			{
				std::filesystem::path Path;
				std::filesystem::file_time_type WriteTime;
			};
			std::vector<FSessionFile> Sessions;
			std::error_code Error;
			for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Directory, Error))
			{
				if (Error || !Entry.is_regular_file()) continue;
				const std::filesystem::path Path = Entry.path();
				const std::string Name = Path.filename().string();
				if (!Name.starts_with(Prefix) || Path.extension() != ".log" || IsRotationStem(Path.stem().string())) continue;
				Sessions.push_back({Path, Entry.last_write_time(Error)});
			}
			if (Sessions.size() < MaxSessions) return;
			std::ranges::sort(Sessions, {}, &FSessionFile::WriteTime);
			const size_t RemoveCount = Sessions.size() - MaxSessions + 1;
			for (size_t Index = 0; Index < RemoveCount; ++Index)
			{
				const std::string BaseStem = Sessions[Index].Path.stem().string();
				for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Directory, Error))
				{
					if (Error) break;
					const std::filesystem::path Candidate = Entry.path();
					const std::string Stem = Candidate.stem().string();
					if (Candidate.extension() == ".log" && (Stem == BaseStem || Stem.starts_with(BaseStem + ".")))
					{
						std::filesystem::remove(Candidate, Error);
						Error.clear();
					}
				}
			}
		}
	}

	class FLogger::FImpl
	{
	public:
		enum class EState { Bootstrap, Running, Stopping, Stopped };

		struct FListenerNode
		{
			FLogListener Callback;
			std::mutex Mutex;
			std::condition_variable Finished;
			bool bEnabled = true;
			uint32 Executing = 0;
		};

		struct FQueuedRecord
		{
			FLogRecord Record;
			bool bNotifyListeners = true;
		};

		auto ShouldLog(ELogLevel Level) const -> bool
		{
			// Session history is intentionally unfiltered; sink thresholds are presentation policies.
			if (HistoryCapacity.load(std::memory_order_relaxed) > 0) return true;
			const int32 NumericLevel = static_cast<int32>(Level);
			if (ListenerCount.load(std::memory_order_relaxed) > 0) return true;
			return NumericLevel >= ConsoleLevel.load(std::memory_order_relaxed) ||
				NumericLevel >= FileLevel.load(std::memory_order_relaxed);
		}

		auto BuildRecord(ELogLevel Level, std::source_location Loc, std::string_view Module, std::string Message) const -> FLogRecord
		{
			FLogRecord Record;
			Record.Timestamp = std::chrono::system_clock::now();
			Record.Level = Level;
			Record.ThreadId = FPlatformLTS::GetCurrentThreadId();
			if (const FRunnableThread* CurrentThread = GetCurrentThread()) Record.ThreadName = CurrentThread->GetThreadName();
			else if (GIsGameThreadIdInitialized && Record.ThreadId == GGameThreadId) Record.ThreadName = "GameThread";
			else Record.ThreadName = "Unknown";
			Record.Module = Module;
			Record.File = Loc.file_name();
			Record.Line = Loc.line();
			Record.Function = Loc.function_name();
			Record.Message = std::move(Message);
			return Record;
		}

		auto Enqueue(FLogRecord Record) -> void
		{
			const bool bReliable = Record.Level >= ELogLevel::Error;
			std::unique_lock Lock(QueueMutex);
			if (State == EState::Bootstrap)
			{
				Record.Sequence = NextSequence++;
				WriteFallback(Record);
				if (BootstrapRecords.size() == DefaultHistoryCapacity)
				{
					BootstrapRecords.pop_front();
					++EvictedBootstrapRecordCount;
				}
				BootstrapRecords.push_back(std::move(Record));
				return;
			}
			if (State != EState::Running)
			{
				WriteFallback(Record);
				return;
			}

			const auto HasSpace = [this] { return State != EState::Running || Queue.size() < Settings.QueueCapacity; };
			if (Queue.size() >= Settings.QueueCapacity)
			{
				if (Record.Level <= ELogLevel::Debug)
				{
					++Dropped[static_cast<size_t>(Record.Level)];
					return;
				}
				if (Record.Level <= ELogLevel::Warn)
				{
					if (!SpaceAvailable.wait_for(Lock, std::chrono::milliseconds(5), HasSpace) || Queue.size() >= Settings.QueueCapacity)
					{
						++Dropped[static_cast<size_t>(Record.Level)];
						return;
					}
				}
				else
				{
					SpaceAvailable.wait(Lock, HasSpace);
				}
			}
			if (State != EState::Running)
			{
				WriteFallback(Record);
				return;
			}
			Record.Sequence = NextSequence++;
			const uint64 Sequence = Record.Sequence;
			LastQueuedSequence = Sequence;
			Queue.push_back({std::move(Record), true});
			WorkAvailable.notify_one();
			if (!bReliable) return;
			Processed.wait(Lock, [this, Sequence] { return LastProcessedSequence >= Sequence || State == EState::Stopped; });
			Lock.unlock();
			FlushSinks();
		}

		auto EnqueueFromDispatcher(FLogRecord Record) -> void
		{
			std::scoped_lock Lock(QueueMutex);
			if (State != EState::Running || Queue.size() >= Settings.QueueCapacity)
			{
				WriteFallback(Record);
				return;
			}
			Record.Sequence = NextSequence++;
			LastQueuedSequence = Record.Sequence;
			Queue.push_back({std::move(Record), false});
			WorkAvailable.notify_one();
		}

		auto DispatchLoop() -> void
		{
			DispatchThreadId.store(FPlatformLTS::GetCurrentThreadId(), std::memory_order_release);
			auto NextFlush = std::chrono::steady_clock::now() + std::chrono::milliseconds(Settings.FlushIntervalMilliseconds);
			for (;;)
			{
				FQueuedRecord QueuedRecord;
				{
					std::unique_lock Lock(QueueMutex);
					if (!WorkAvailable.wait_until(Lock, NextFlush, [this] {
						return !BootstrapRecords.empty() || !Queue.empty() || State == EState::Stopping;
					}))
					{
						Lock.unlock();
						FlushSinks();
						NextFlush = std::chrono::steady_clock::now() + std::chrono::milliseconds(Settings.FlushIntervalMilliseconds);
						continue;
					}
					if (BootstrapRecords.empty() && Queue.empty() && State == EState::Stopping) break;
					if (!BootstrapRecords.empty())
					{
						QueuedRecord = {std::move(BootstrapRecords.front()), true};
						BootstrapRecords.pop_front();
					}
					else
					{
						QueuedRecord = std::move(Queue.front());
						Queue.pop_front();
						SpaceAvailable.notify_all();
					}
				}

				ProcessRecord(QueuedRecord.Record, QueuedRecord.bNotifyListeners);
				{
					std::scoped_lock Lock(QueueMutex);
					LastProcessedSequence = QueuedRecord.Record.Sequence;
					QueueDropSummaryLocked();
				}
				Processed.notify_all();
				if (std::chrono::steady_clock::now() >= NextFlush)
				{
					FlushSinks();
					NextFlush = std::chrono::steady_clock::now() + std::chrono::milliseconds(Settings.FlushIntervalMilliseconds);
				}
			}
			FlushSinks();
			{
				std::scoped_lock Lock(QueueMutex);
				State = EState::Stopped;
			}
			DispatchThreadId.store(0, std::memory_order_release);
			Processed.notify_all();
			SpaceAvailable.notify_all();
		}

		auto QueueDropSummaryLocked() -> void
		{
			uint64 Total = 0;
			for (uint64 Count : Dropped) Total += Count;
			if (Total == 0) return;
			FLogRecord Summary;
			Summary.Sequence = NextSequence++;
			LastQueuedSequence = Summary.Sequence;
			Summary.Timestamp = std::chrono::system_clock::now();
			Summary.Level = ELogLevel::Warn;
			Summary.ThreadId = FPlatformLTS::GetCurrentThreadId();
			Summary.ThreadName = "LogDispatcher";
			Summary.Module = "Core";
			Summary.Message = std::format("Dropped {} log records (trace {}, debug {}, info {}, warn {}) because the async queue was full.",
				Total, Dropped[0], Dropped[1], Dropped[2], Dropped[3]);
			Dropped.fill(0);
			Queue.push_back({std::move(Summary), true});
			WorkAvailable.notify_one();
		}

		auto AppendHistory(const FLogRecord& Record) -> void
		{
			std::scoped_lock Lock(HistoryMutex);
			if (History.size() == Settings.HistoryCapacity) History.pop_front();
			History.push_back(Record);
		}

		auto ReadRecords(uint64 NextSequence, uint32 MaxRecords) const -> FLogReadResult
		{
			FLogReadResult Result;
			Result.NextSequence = NextSequence;
			const uint32 BatchSize = std::clamp(MaxRecords, MinReadBatchSize, MaxReadBatchSize);
			std::scoped_lock Lock(HistoryMutex);
			if (History.empty()) return Result;

			Result.OldestAvailableSequence = History.front().Sequence;
			Result.NewestAvailableSequence = History.back().Sequence;
			uint64 ReadSequence = NextSequence;
			if (ReadSequence < Result.OldestAvailableSequence)
			{
				Result.EvictedRecordCount = Result.OldestAvailableSequence - ReadSequence;
				ReadSequence = Result.OldestAvailableSequence;
			}
			const auto First = std::ranges::lower_bound(History, ReadSequence, {}, &FLogRecord::Sequence);
			const size_t Available = static_cast<size_t>(std::distance(First, History.end()));
			const size_t Count = std::min<size_t>(Available, BatchSize);
			Result.Records.reserve(Count);
			for (auto Iterator = First; Iterator != History.end() && Result.Records.size() < Count; ++Iterator)
			{
				Result.Records.push_back(*Iterator);
			}
			if (!Result.Records.empty()) Result.NextSequence = Result.Records.back().Sequence + 1;
			return Result;
		}

		auto ProcessRecord(const FLogRecord& Record, bool bNotifyListeners) -> void
		{
			AppendHistory(Record);
			try
			{
				const spdlog::source_loc Source{Record.File.c_str(), static_cast<int>(Record.Line), Record.Function.c_str()};
				if (ConsoleLogger && static_cast<int32>(Record.Level) >= ConsoleLevel.load(std::memory_order_relaxed))
				{
					const std::string Payload = std::format("[{}] {}", Record.Module, Record.Message);
					ConsoleLogger->log(Record.Timestamp, Source, ToSpdLevel(Record.Level),
						spdlog::string_view_t(Payload.data(), Payload.size()));
				}
				if (FileLogger && static_cast<int32>(Record.Level) >= FileLevel.load(std::memory_order_relaxed))
				{
					const std::string Payload = std::format("[#{}][{}:{}][{}] {}", Record.Sequence,
						Record.ThreadId, Record.ThreadName, Record.Module, Record.Message);
					FileLogger->log(Record.Timestamp, Source, ToSpdLevel(Record.Level),
						spdlog::string_view_t(Payload.data(), Payload.size()));
				}
			}
			catch (const std::exception& Error)
			{
				WriteFallbackText(std::format("Logger sink failure: {}", Error.what()));
			}
			if (Record.Level >= ELogLevel::Error) FlushSinks();
			if (bNotifyListeners) NotifyListeners(Record);
		}

		auto NotifyListeners(const FLogRecord& Record) -> void
		{
			std::vector<std::shared_ptr<FListenerNode>> Snapshot;
			{
				std::scoped_lock Lock(ListenerMutex);
				Snapshot.reserve(Listeners.size());
				for (const auto& [Handle, Listener] : Listeners)
				{
					(void)Handle;
					Snapshot.push_back(Listener);
				}
			}
			for (const std::shared_ptr<FListenerNode>& Listener : Snapshot)
			{
				{
					std::scoped_lock Lock(Listener->Mutex);
					if (!Listener->bEnabled) continue;
					++Listener->Executing;
				}
				try
				{
					Listener->Callback(Record);
				}
				catch (const std::exception& Error)
				{
					WriteFallbackText(std::format("Log listener threw an exception: {}", Error.what()));
				}
				catch (...)
				{
					WriteFallbackText("Log listener threw an unknown exception.");
				}
				{
					std::scoped_lock Lock(Listener->Mutex);
					--Listener->Executing;
				}
				Listener->Finished.notify_all();
			}
		}

		auto FlushSinks() const -> void
		{
			if (ConsoleLogger) ConsoleLogger->flush();
			if (FileLogger) FileLogger->flush();
		}

		auto WriteFallback(const FLogRecord& Record) const -> void
		{
			WriteFallbackText(std::format("[{}][{}] {}", LevelName(Record.Level), Record.Module, Record.Message));
		}

		auto WriteFallbackText(std::string_view Text) const -> void
		{
			std::scoped_lock Lock(FallbackMutex);
			std::fprintf(stderr, "%.*s\n", static_cast<int>(Text.size()), Text.data());
			std::fflush(stderr);
		}

		mutable std::mutex QueueMutex;
		std::mutex LifecycleMutex;
		std::condition_variable WorkAvailable;
		std::condition_variable SpaceAvailable;
		std::condition_variable Processed;
		std::deque<FQueuedRecord> Queue;
		std::deque<FLogRecord> BootstrapRecords;
		uint64 EvictedBootstrapRecordCount = 0;
		std::array<uint64, 6> Dropped{};
		uint64 NextSequence = 1;
		uint64 LastQueuedSequence = 0;
		uint64 LastProcessedSequence = 0;
		EState State = EState::Bootstrap;
		FLogSettings Settings;
		std::jthread DispatchThread;
		std::atomic<uint32> DispatchThreadId = 0;
		std::atomic<uint32> HistoryCapacity = DefaultHistoryCapacity;

		mutable std::mutex HistoryMutex;
		std::deque<FLogRecord> History;

		std::shared_ptr<spdlog::logger> ConsoleLogger;
		std::shared_ptr<spdlog::logger> FileLogger;
		std::atomic<int32> ConsoleLevel{static_cast<int32>(ELogLevel::Debug)};
		std::atomic<int32> FileLevel{static_cast<int32>(ELogLevel::Trace)};

		std::mutex ListenerMutex;
		std::unordered_map<FLogListenerHandle, std::shared_ptr<FListenerNode>> Listeners;
		std::atomic<FLogListenerHandle> NextListenerHandle = 1;
		std::atomic<uint32> ListenerCount = 0;
		mutable std::mutex FallbackMutex;
	};

	FLogger::FLogger() : Impl(std::make_unique<FImpl>())
	{
	}

	FLogger::~FLogger()
	{
		Shutdown();
	}

	auto FLogger::Get() -> FLogger&
	{
		static FLogger Instance;
		return Instance;
	}

	auto FLogger::ShouldLog(ELogLevel Level) const -> bool
	{
		return Impl->ShouldLog(Level);
	}

	auto FLogger::LogInternal(ELogLevel Level, std::source_location Loc, std::string_view Module, std::string_view Fmt, std::format_args Args) const -> void
	{
		std::string Message;
		try
		{
			Message = std::vformat(Fmt, Args);
		}
		catch (const std::exception& Error)
		{
			Message = std::format("Log formatting failed: {} (format: {})", Error.what(), Fmt);
		}
		FLogRecord Record = Impl->BuildRecord(Level, Loc, Module, std::move(Message));
		if (FPlatformLTS::GetCurrentThreadId() == Impl->DispatchThreadId.load(std::memory_order_acquire))
		{
			Impl->EnqueueFromDispatcher(std::move(Record));
			return;
		}
		Impl->Enqueue(std::move(Record));
	}

	auto FLogger::SetConsoleLogLevel(ELogLevel Level) -> void
	{
		Impl->ConsoleLevel.store(static_cast<int32>(Level), std::memory_order_relaxed);
		if (Impl->ConsoleLogger) Impl->ConsoleLogger->set_level(ToSpdLevel(Level));
	}

	auto FLogger::ReadRecords(uint64 NextSequence, uint32 MaxRecords) const -> FLogReadResult
	{
		return Impl->ReadRecords(NextSequence, MaxRecords);
	}

	auto FLogger::AddListener(FLogListener Listener) -> FLogListenerHandle
	{
		if (!Listener) return 0;
		const FLogListenerHandle Handle = Impl->NextListenerHandle.fetch_add(1, std::memory_order_relaxed);
		auto Node = std::make_shared<FImpl::FListenerNode>();
		Node->Callback = std::move(Listener);
		{
			std::scoped_lock Lock(Impl->ListenerMutex);
			Impl->Listeners.emplace(Handle, std::move(Node));
		}
		Impl->ListenerCount.fetch_add(1, std::memory_order_relaxed);
		return Handle;
	}

	auto FLogger::RemoveListener(FLogListenerHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::shared_ptr<FImpl::FListenerNode> Node;
		{
			std::scoped_lock Lock(Impl->ListenerMutex);
			const auto Iterator = Impl->Listeners.find(Handle);
			if (Iterator == Impl->Listeners.end()) return;
			Node = std::move(Iterator->second);
			Impl->Listeners.erase(Iterator);
		}
		Impl->ListenerCount.fetch_sub(1, std::memory_order_relaxed);
		std::unique_lock Lock(Node->Mutex);
		Node->bEnabled = false;
		if (FPlatformLTS::GetCurrentThreadId() != Impl->DispatchThreadId.load(std::memory_order_acquire))
		{
			Node->Finished.wait(Lock, [&Node] { return Node->Executing == 0; });
		}
	}

	auto FLogger::Initialize(const FLogSettings& InSettings) -> bool
	{
		std::scoped_lock LifecycleLock(Impl->LifecycleMutex);
		{
			std::scoped_lock Lock(Impl->QueueMutex);
			if (Impl->State == FImpl::EState::Running) return true;
			if (Impl->State == FImpl::EState::Stopping) return false;
		}

		FLogSettings Settings = InSettings;
		Settings.QueueCapacity = std::clamp(Settings.QueueCapacity, 256u, 1024u * 1024u);
		Settings.HistoryCapacity = std::clamp(Settings.HistoryCapacity, MinHistoryCapacity, MaxHistoryCapacity);
		Settings.FlushIntervalMilliseconds = std::clamp(Settings.FlushIntervalMilliseconds, 50u, 60000u);
		Settings.MaxFileSizeBytes = std::clamp<uint64>(Settings.MaxFileSizeBytes, 1024ull, 1024ull * 1024ull * 1024ull);
		Settings.MaxFilesPerSession = std::clamp(Settings.MaxFilesPerSession, 1u, 100u);
		Settings.MaxSessions = std::clamp(Settings.MaxSessions, 1u, 1000u);
		if (Settings.LogDirectory.empty()) Settings.LogDirectory = (std::filesystem::path(FPaths::LaunchDir()) / "Logs").string();
		Settings.ProfileName = SanitizeFileComponent(Settings.ProfileName);

		std::shared_ptr<spdlog::logger> ConsoleLogger;
		std::shared_ptr<spdlog::logger> FileLogger;
		std::string FileFailure;
		try
		{
			auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			ConsoleSink->set_level(ToSpdLevel(Settings.ConsoleLevel));
			ConsoleSink->set_pattern("[%H:%M:%S][%^%l%$]%v");
			ConsoleLogger = std::make_shared<spdlog::logger>("DurinConsole", ConsoleSink);
			ConsoleLogger->set_level(spdlog::level::trace);
		}
		catch (const std::exception& Error)
		{
			Impl->WriteFallbackText(std::format("Failed to initialize console logger: {}", Error.what()));
		}

		try
		{
			const std::filesystem::path Directory(Settings.LogDirectory);
			std::filesystem::create_directories(Directory);
			const std::string Prefix = "Durin-" + Settings.ProfileName + "-";
			CleanupOldSessions(Directory, Prefix, Settings.MaxSessions);
			const std::string FileName = std::format("{}{}-{}.log", Prefix, SessionTimestamp(), FPlatformProcess::CurrentProcessId());
			auto FileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
				(Directory / FileName).string(), static_cast<size_t>(Settings.MaxFileSizeBytes), Settings.MaxFilesPerSession - 1, false);
			FileSink->set_level(ToSpdLevel(Settings.FileLevel));
			FileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l][%s:%#] %v");
			FileLogger = std::make_shared<spdlog::logger>("DurinFile", FileSink);
			FileLogger->set_level(spdlog::level::trace);
			FileLogger->flush_on(spdlog::level::err);
		}
		catch (const std::exception& Error)
		{
			FileFailure = Error.what();
			Impl->WriteFallbackText(std::format("File logging disabled: {}", FileFailure));
		}

		const auto ErrorHandler = [Impl = Impl.get()](const std::string& Error) {
			Impl->WriteFallbackText(std::format("spdlog error: {}", Error));
		};
		if (ConsoleLogger) ConsoleLogger->set_error_handler(ErrorHandler);
		if (FileLogger) FileLogger->set_error_handler(ErrorHandler);

		{
			std::scoped_lock Lock(Impl->QueueMutex);
			if (Impl->State == FImpl::EState::Stopped)
			{
				Impl->Queue.clear();
				Impl->BootstrapRecords.clear();
				Impl->Dropped.fill(0);
				Impl->EvictedBootstrapRecordCount = 0;
				Impl->NextSequence = 1;
				Impl->LastQueuedSequence = 0;
				Impl->LastProcessedSequence = 0;
				std::scoped_lock HistoryLock(Impl->HistoryMutex);
				Impl->History.clear();
			}
			Impl->Settings = std::move(Settings);
			Impl->HistoryCapacity.store(Impl->Settings.HistoryCapacity, std::memory_order_relaxed);
			Impl->ConsoleLogger = std::move(ConsoleLogger);
			Impl->FileLogger = std::move(FileLogger);
			Impl->ConsoleLevel.store(static_cast<int32>(Impl->Settings.ConsoleLevel), std::memory_order_relaxed);
			Impl->FileLevel.store(static_cast<int32>(Impl->Settings.FileLevel), std::memory_order_relaxed);
			if (!Impl->BootstrapRecords.empty()) Impl->LastQueuedSequence = Impl->BootstrapRecords.back().Sequence;
			if (Impl->EvictedBootstrapRecordCount > 0)
			{
				FLogRecord Warning = Impl->BuildRecord(ELogLevel::Warn, std::source_location::current(), "Core",
					std::format("Discarded {} bootstrap log records because pre-initialization history exceeded {} records.",
						Impl->EvictedBootstrapRecordCount, DefaultHistoryCapacity));
				Warning.Sequence = Impl->NextSequence++;
				Impl->LastQueuedSequence = Warning.Sequence;
				Impl->Queue.push_back({std::move(Warning), true});
				Impl->EvictedBootstrapRecordCount = 0;
			}
			if (!FileFailure.empty())
			{
				FLogRecord Warning = Impl->BuildRecord(ELogLevel::Warn, std::source_location::current(), "Core",
					std::format("File logging disabled: {}", FileFailure));
				Warning.Sequence = Impl->NextSequence++;
				Impl->LastQueuedSequence = Warning.Sequence;
				Impl->Queue.push_back({std::move(Warning), true});
			}
			Impl->State = FImpl::EState::Running;
			Impl->DispatchThread = std::jthread([Impl = Impl.get()] { Impl->DispatchLoop(); });
		}
		Impl->WorkAvailable.notify_one();
		return true;
	}

	auto FLogger::Initialize() -> void
	{
		FLogSettings Settings;
#if DURIN_BUILD_SHIPPING
		Settings.ConsoleLevel = ELogLevel::Info;
		Settings.FileLevel = ELogLevel::Info;
#endif
		Settings.LogDirectory = (std::filesystem::path(FPaths::LaunchDir()) / "Logs").string();
		Settings.ProfileName = DURIN_PROFILE_NAME;
		(void)Initialize(Settings);
	}

	auto FLogger::Flush() -> void
	{
		std::unique_lock Lock(Impl->QueueMutex);
		if (Impl->State != FImpl::EState::Running)
		{
			Lock.unlock();
			Impl->FlushSinks();
			return;
		}
		Impl->QueueDropSummaryLocked();
		const uint64 Target = Impl->LastQueuedSequence;
		Impl->Processed.wait(Lock, [this, Target] { return Impl->LastProcessedSequence >= Target || Impl->State != FImpl::EState::Running; });
		Lock.unlock();
		Impl->FlushSinks();
	}

	auto FLogger::Shutdown() -> void
	{
		if (FPlatformLTS::GetCurrentThreadId() == Impl->DispatchThreadId.load(std::memory_order_acquire))
		{
			{
				std::scoped_lock Lock(Impl->QueueMutex);
				if (Impl->State == FImpl::EState::Running) Impl->State = FImpl::EState::Stopping;
			}
			Impl->WorkAvailable.notify_all();
			Impl->SpaceAvailable.notify_all();
			return;
		}
		std::scoped_lock LifecycleLock(Impl->LifecycleMutex);
		bool bNotifyWorker = false;
		{
			std::scoped_lock Lock(Impl->QueueMutex);
			if (Impl->State == FImpl::EState::Bootstrap)
			{
				Impl->State = FImpl::EState::Stopped;
				Impl->BootstrapRecords.clear();
				return;
			}
			if (Impl->State == FImpl::EState::Stopped)
			{
				std::scoped_lock HistoryLock(Impl->HistoryMutex);
				Impl->History.clear();
				return;
			}
			if (Impl->State == FImpl::EState::Running)
			{
				Impl->State = FImpl::EState::Stopping;
				bNotifyWorker = true;
			}
		}
		if (bNotifyWorker)
		{
			Impl->WorkAvailable.notify_all();
			Impl->SpaceAvailable.notify_all();
		}
		if (Impl->DispatchThread.joinable()) Impl->DispatchThread.join();
		Impl->FlushSinks();
		{
			std::scoped_lock Lock(Impl->QueueMutex);
			Impl->State = FImpl::EState::Stopped;
		}
		{
			std::scoped_lock Lock(Impl->HistoryMutex);
			Impl->History.clear();
		}
		Impl->Processed.notify_all();
	}

	auto StringToLogLevel(std::string_view InLogLevel, ELogLevel DefaultLevel) -> ELogLevel
	{
		ELogLevel Level = DefaultLevel;
		return TryParseLogLevel(InLogLevel, Level) ? Level : DefaultLevel;
	}

	auto LoggerInit() -> void
	{
		FLogSettings Settings;
#if DURIN_BUILD_SHIPPING
		Settings.ConsoleLevel = ELogLevel::Info;
		Settings.FileLevel = ELogLevel::Info;
#endif
		Settings.LogDirectory = (std::filesystem::path(FPaths::LaunchDir()) / "Logs").string();
		Settings.ProfileName = DURIN_PROFILE_NAME;

		std::vector<std::string> InvalidLevels;
		const FYamlNodeView Logging = GetModuleConfig("Core").GetView("Logging");
		const auto ReadLevel = [&InvalidLevels](const FYamlNodeView& Node, ELogLevel DefaultLevel, std::string_view Name) {
			if (!Node.IsValid()) return DefaultLevel;
			ELogLevel Level = DefaultLevel;
			const std::string Text = Node.GetString();
			if (!TryParseLogLevel(Text, Level)) InvalidLevels.push_back(std::format("{}={}", Name, Text));
			return Level;
		};
		if (Logging.IsMap())
		{
			Settings.ConsoleLevel = ReadLevel(Logging.GetView("ConsoleLevel"), Settings.ConsoleLevel, "Logging.ConsoleLevel");
			Settings.FileLevel = ReadLevel(Logging.GetView("FileLevel"), Settings.FileLevel, "Logging.FileLevel");
			Settings.QueueCapacity = static_cast<uint32>(std::min<uint64>(Logging.GetView("QueueCapacity").GetUInt(Settings.QueueCapacity), 1024ull * 1024ull));
			Settings.HistoryCapacity = static_cast<uint32>(std::min<uint64>(Logging.GetView("HistoryCapacity").GetUInt(Settings.HistoryCapacity), MaxHistoryCapacity));
			Settings.FlushIntervalMilliseconds = static_cast<uint32>(std::min<uint64>(Logging.GetView("FlushIntervalMilliseconds").GetUInt(Settings.FlushIntervalMilliseconds), 60000));
			const uint64 MaxFileSizeMiB = std::min<uint64>(Logging.GetView("MaxFileSizeMiB").GetUInt(Settings.MaxFileSizeBytes / (1024ull * 1024ull)), 1024);
			Settings.MaxFileSizeBytes = MaxFileSizeMiB * 1024ull * 1024ull;
			Settings.MaxFilesPerSession = static_cast<uint32>(std::min<uint64>(Logging.GetView("MaxFilesPerSession").GetUInt(Settings.MaxFilesPerSession), 100));
			Settings.MaxSessions = static_cast<uint32>(std::min<uint64>(Logging.GetView("MaxSessions").GetUInt(Settings.MaxSessions), 1000));
		}

		FLogger& Logger = FLogger::Get();
		Logger.Initialize(Settings);
		for (const std::string& Invalid : InvalidLevels) DURIN_WARN("Invalid log level {}; using the configured default.", Invalid);
	}

	auto LoggerShutdown() -> void
	{
		FLogger::Get().Shutdown();
	}
} // namespace Durin
