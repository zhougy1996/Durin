#include "Logging/Logger.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeTestDirectory(std::string_view Suffix) -> std::filesystem::path
	{
		const std::filesystem::path Directory = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LoggerTests" / Suffix;
		std::error_code Error;
		std::filesystem::remove_all(Directory, Error);
		std::filesystem::create_directories(Directory);
		return Directory;
	}

	auto MakeSettings(const std::filesystem::path& Directory) -> Durin::FLogSettings
	{
		Durin::FLogSettings Settings;
		Settings.ConsoleLevel = Durin::ELogLevel::Fatal;
		Settings.FileLevel = Durin::ELogLevel::Trace;
		Settings.LogDirectory = Directory.string();
		Settings.ProfileName = "CoreTests";
		Settings.FlushIntervalMilliseconds = 50;
		return Settings;
	}

	auto FindLogFiles(const std::filesystem::path& Directory) -> std::vector<std::filesystem::path>
	{
		std::vector<std::filesystem::path> Files;
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Directory))
		{
			if (Entry.is_regular_file() && Entry.path().extension() == ".log") Files.push_back(Entry.path());
		}
		return Files;
	}

	auto ReadFile(const std::filesystem::path& Path) -> std::string
	{
		std::ifstream Stream(Path, std::ios::binary);
		return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	}

	auto ReadAll(Durin::FLogger& Logger, uint64_t NextSequence = 1, uint32_t BatchSize = 512) -> std::vector<Durin::FLogRecord>
	{
		std::vector<Durin::FLogRecord> Records;
		for (;;)
		{
			Durin::FLogReadResult Read = Logger.ReadRecords(NextSequence, BatchSize);
			if (Read.Records.empty()) break;
			NextSequence = Read.NextSequence;
			Records.insert(Records.end(), std::make_move_iterator(Read.Records.begin()), std::make_move_iterator(Read.Records.end()));
		}
		return Records;
	}
}

TEST(FLoggerTests, EmptyHistoryPreservesTheRequestedCursor)
{
	Durin::FLogger Logger;
	const Durin::FLogReadResult Read = Logger.ReadRecords(37, 0);
	EXPECT_TRUE(Read.Records.empty());
	EXPECT_EQ(Read.OldestAvailableSequence, 0u);
	EXPECT_EQ(Read.NewestAvailableSequence, 0u);
	EXPECT_EQ(Read.NextSequence, 37u);
	EXPECT_EQ(Read.EvictedRecordCount, 0u);
}

TEST(FLoggerTests, RetainsBootstrapAndRuntimeRecordsForLateReaders)
{
	Durin::FLogger Logger;
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "Bootstrap", "First startup record");
	Logger.Log(Durin::ELogLevel::Debug, std::source_location::current(), "Bootstrap", "Second startup record");
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("HistoryStartup"))));
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Runtime record");
	Logger.Flush();

	const std::vector<Durin::FLogRecord> Records = ReadAll(Logger);
	ASSERT_EQ(Records.size(), 3u);
	EXPECT_EQ(Records[0].Message, "First startup record");
	EXPECT_EQ(Records[1].Message, "Second startup record");
	EXPECT_EQ(Records[2].Message, "Runtime record");
	EXPECT_TRUE(std::ranges::is_sorted(Records, {}, &Durin::FLogRecord::Sequence));
}

TEST(FLoggerTests, ReadsHistoryInBoundedBatches)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("HistoryBatches"))));
	for (int Index = 0; Index < 25; ++Index)
	{
		Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Batch {}", Index);
	}
	Logger.Flush();

	uint64_t Cursor = 1;
	std::vector<Durin::FLogRecord> Records;
	for (int BatchIndex = 0; BatchIndex < 4; ++BatchIndex)
	{
		Durin::FLogReadResult Read = Logger.ReadRecords(Cursor, 7);
		EXPECT_LE(Read.Records.size(), 7u);
		Cursor = Read.NextSequence;
		Records.insert(Records.end(), Read.Records.begin(), Read.Records.end());
	}
	ASSERT_EQ(Records.size(), 25u);
	EXPECT_EQ(Cursor, Records.back().Sequence + 1);
	EXPECT_TRUE(Logger.ReadRecords(Cursor, 7).Records.empty());
}

TEST(FLoggerTests, ReportsHistoryEvictionGaps)
{
	Durin::FLogger Logger;
	Durin::FLogSettings Settings = MakeSettings(MakeTestDirectory("HistoryEviction"));
	Settings.HistoryCapacity = 1; // Clamped to the documented minimum.
	ASSERT_TRUE(Logger.Initialize(Settings));
	for (int Index = 0; Index < 300; ++Index)
	{
		Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Evict {}", Index);
	}
	Logger.Flush();

	const Durin::FLogReadResult Read = Logger.ReadRecords(1, 4096);
	ASSERT_EQ(Read.Records.size(), 256u);
	EXPECT_EQ(Read.OldestAvailableSequence, 45u);
	EXPECT_EQ(Read.NewestAvailableSequence, 300u);
	EXPECT_EQ(Read.EvictedRecordCount, 44u);
	EXPECT_EQ(Read.NextSequence, 301u);
}

TEST(FLoggerTests, PreservesOrderedHistoryDuringConcurrentProductionAndReads)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("ConcurrentHistory"))));
	std::atomic<bool> bStopReader = false;
	auto Reader = std::async(std::launch::async, [&] {
		std::vector<uint64_t> Sequences;
		uint64_t Cursor = 1;
		while (!bStopReader.load(std::memory_order_acquire))
		{
			Durin::FLogReadResult Read = Logger.ReadRecords(Cursor, 17);
			Cursor = Read.NextSequence;
			for (const Durin::FLogRecord& Record : Read.Records) Sequences.push_back(Record.Sequence);
			if (Read.Records.empty()) std::this_thread::yield();
		}
		for (;;)
		{
			Durin::FLogReadResult Read = Logger.ReadRecords(Cursor, 17);
			if (Read.Records.empty()) break;
			Cursor = Read.NextSequence;
			for (const Durin::FLogRecord& Record : Read.Records) Sequences.push_back(Record.Sequence);
		}
		return Sequences;
	});

	std::vector<std::thread> Producers;
	for (int ThreadIndex = 0; ThreadIndex < 4; ++ThreadIndex)
	{
		Producers.emplace_back([&Logger, ThreadIndex] {
			for (int MessageIndex = 0; MessageIndex < 100; ++MessageIndex)
			{
				Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "{}:{}", ThreadIndex, MessageIndex);
			}
		});
	}
	for (std::thread& Producer : Producers) Producer.join();
	Logger.Flush();
	bStopReader.store(true, std::memory_order_release);
	const std::vector<uint64_t> Sequences = Reader.get();

	ASSERT_EQ(Sequences.size(), 400u);
	EXPECT_TRUE(std::ranges::is_sorted(Sequences));
	EXPECT_EQ(std::unordered_set<uint64_t>(Sequences.begin(), Sequences.end()).size(), Sequences.size());
}

TEST(FLoggerTests, DeliversOwnedStructuredRecordsAndUnregistersListeners)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("Structured"))));
	std::mutex Mutex;
	std::vector<Durin::FLogRecord> Records;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		std::scoped_lock Lock(Mutex);
		Records.push_back(Record);
	});

	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Value {}", 42);
	Logger.Flush();
	Logger.RemoveListener(Handle);
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Ignored");
	Logger.Flush();

	std::scoped_lock Lock(Mutex);
	ASSERT_EQ(Records.size(), 1u);
	EXPECT_GT(Records[0].Sequence, 0u);
	EXPECT_EQ(Records[0].Level, Durin::ELogLevel::Info);
	EXPECT_EQ(Records[0].Module, "LoggerTests");
	EXPECT_EQ(Records[0].Message, "Value 42");
	EXPECT_FALSE(Records[0].ThreadName.empty());
	EXPECT_GT(Records[0].ThreadId, 0u);
	EXPECT_FALSE(Records[0].File.empty());
	EXPECT_GT(Records[0].Line, 0u);
	EXPECT_FALSE(Records[0].Function.empty());
}

TEST(FLoggerTests, SupportsConcurrentProducersAndOrderedDispatch)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("Concurrent"))));
	std::mutex Mutex;
	std::vector<uint64_t> Sequences;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		std::scoped_lock Lock(Mutex);
		Sequences.push_back(Record.Sequence);
	});

	std::vector<std::thread> Threads;
	for (int ThreadIndex = 0; ThreadIndex < 4; ++ThreadIndex)
	{
		Threads.emplace_back([&Logger, ThreadIndex]() {
			for (int MessageIndex = 0; MessageIndex < 25; ++MessageIndex)
			{
				Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "{}:{}", ThreadIndex, MessageIndex);
			}
		});
	}
	for (std::thread& Thread : Threads) Thread.join();
	Logger.Flush();
	Logger.RemoveListener(Handle);

	std::scoped_lock Lock(Mutex);
	ASSERT_EQ(Sequences.size(), 100u);
	EXPECT_TRUE(std::ranges::is_sorted(Sequences));
	EXPECT_EQ(std::unordered_set<uint64_t>(Sequences.begin(), Sequences.end()).size(), Sequences.size());
}

TEST(FLoggerTests, LateListenerOmitsRecordsAlreadyProcessedBeforeRegistration)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("LateListenerProcessed"))));
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Before registration");
	Logger.Flush();

	std::vector<std::string> Messages;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		Messages.push_back(Record.Message);
	});
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "After registration");
	Logger.Flush();
	Logger.RemoveListener(Handle);

	ASSERT_EQ(Messages.size(), 1u);
	EXPECT_EQ(Messages.front(), "After registration");
}

TEST(FLoggerTests, LateListenerReceivesRecordsQueuedBeforeRegistration)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("LateListenerQueued"))));
	std::mutex Mutex;
	std::condition_variable Entered;
	std::condition_variable Release;
	bool bEntered = false;
	bool bRelease = false;
	const Durin::FLogListenerHandle BlockingHandle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		if (Record.Message != "Block dispatcher") return;
		std::unique_lock Lock(Mutex);
		bEntered = true;
		Entered.notify_one();
		Release.wait(Lock, [&] { return bRelease; });
	});
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Block dispatcher");
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Entered.wait_for(Lock, std::chrono::seconds(2), [&] { return bEntered; }));
	}

	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Queued before registration");
	std::vector<std::string> LateMessages;
	const Durin::FLogListenerHandle LateHandle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		LateMessages.push_back(Record.Message);
	});
	{
		std::scoped_lock Lock(Mutex);
		bRelease = true;
	}
	Release.notify_one();
	Logger.Flush();
	Logger.RemoveListener(LateHandle);
	Logger.RemoveListener(BlockingHandle);

	ASSERT_EQ(LateMessages.size(), 1u);
	EXPECT_EQ(LateMessages.front(), "Queued before registration");
}

TEST(FLoggerTests, ErrorReturnsOnlyAfterFileIsFlushed)
{
	const std::filesystem::path Directory = MakeTestDirectory("ReliableFile");
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(Directory)));
	Logger.Log(Durin::ELogLevel::Error, std::source_location::current(), "LoggerTests", "Reliable {}", 17);

	const std::vector<std::filesystem::path> Files = FindLogFiles(Directory);
	ASSERT_EQ(Files.size(), 1u);
	const std::string Contents = ReadFile(Files.front());
	EXPECT_NE(Contents.find("[LoggerTests] Reliable 17"), std::string::npos);
	EXPECT_NE(Contents.find("[#"), std::string::npos);
}

TEST(FLoggerTests, ReliableLoggingCurrentlyWaitsForListenerCompletion)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("ReliableListenerDelay"))));
	std::mutex Mutex;
	std::condition_variable Entered;
	std::condition_variable Release;
	bool bEntered = false;
	bool bRelease = false;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		if (Record.Message != "Reliable block") return;
		std::unique_lock Lock(Mutex);
		bEntered = true;
		Entered.notify_one();
		Release.wait(Lock, [&] { return bRelease; });
	});

	auto Logging = std::async(std::launch::async, [&] {
		Logger.Log(Durin::ELogLevel::Error, std::source_location::current(), "LoggerTests", "Reliable block");
	});
	bool bListenerEntered = false;
	{
		std::unique_lock Lock(Mutex);
		bListenerEntered = Entered.wait_for(Lock, std::chrono::seconds(2), [&] { return bEntered; });
	}
	if (bListenerEntered)
	{
		EXPECT_EQ(Logging.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
	}
	{
		std::scoped_lock Lock(Mutex);
		bRelease = true;
	}
	Release.notify_one();
	EXPECT_EQ(Logging.wait_for(std::chrono::seconds(2)), std::future_status::ready);
	EXPECT_TRUE(bListenerEntered);
	Logger.RemoveListener(Handle);
}

TEST(FLoggerTests, RemoveListenerWaitsForExecutingCallback)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("SafeRemove"))));
	std::mutex Mutex;
	std::condition_variable Entered;
	std::condition_variable Release;
	bool bEntered = false;
	bool bRelease = false;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord&) {
		std::unique_lock Lock(Mutex);
		bEntered = true;
		Entered.notify_one();
		Release.wait(Lock, [&] { return bRelease; });
	});
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Block");
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Entered.wait_for(Lock, std::chrono::seconds(2), [&] { return bEntered; }));
	}

	auto Removal = std::async(std::launch::async, [&] { Logger.RemoveListener(Handle); });
	EXPECT_EQ(Removal.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
	{
		std::scoped_lock Lock(Mutex);
		bRelease = true;
	}
	Release.notify_one();
	EXPECT_EQ(Removal.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

TEST(FLoggerTests, ListenerCanRemoveItselfThrowAndLogRecursively)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("ListenerEdges"))));
	std::atomic<int32_t> SelfCount = 0;
	Durin::FLogListenerHandle SelfHandle = 0;
	SelfHandle = Logger.AddListener([&](const Durin::FLogRecord&) {
		SelfCount.fetch_add(1, std::memory_order_relaxed);
		Logger.RemoveListener(SelfHandle);
		Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "Recursive", "Nested");
	});
	const Durin::FLogListenerHandle ThrowingHandle = Logger.AddListener([](const Durin::FLogRecord&) {
		throw std::runtime_error("expected test exception");
	});

	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "First");
	Logger.Flush();
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Second");
	Logger.Flush();
	Logger.RemoveListener(ThrowingHandle);
	EXPECT_EQ(SelfCount.load(std::memory_order_relaxed), 1);
}

TEST(FLoggerTests, RecursiveLogsRemainOrderedBehindAlreadyQueuedRecords)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("RecursiveHistoryOrder"))));
	std::mutex Mutex;
	std::condition_variable Entered;
	std::condition_variable Release;
	bool bEntered = false;
	bool bRelease = false;
	bool bNestedQueued = false;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		if (Record.Message != "Outer") return;
		std::unique_lock Lock(Mutex);
		bEntered = true;
		Entered.notify_one();
		Release.wait(Lock, [&] { return bRelease; });
		Lock.unlock();
		Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "Recursive", "Nested");
		Lock.lock();
		bNestedQueued = true;
		Entered.notify_one();
	});
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Outer");
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Entered.wait_for(Lock, std::chrono::seconds(2), [&] { return bEntered; }));
	}
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Already queued");
	{
		std::scoped_lock Lock(Mutex);
		bRelease = true;
	}
	Release.notify_one();
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Entered.wait_for(Lock, std::chrono::seconds(2), [&] { return bNestedQueued; }));
	}
	Logger.Flush();
	Logger.RemoveListener(Handle);

	const std::vector<Durin::FLogRecord> Records = ReadAll(Logger);
	ASSERT_EQ(Records.size(), 3u);
	EXPECT_EQ(Records[0].Message, "Outer");
	EXPECT_EQ(Records[1].Message, "Already queued");
	EXPECT_EQ(Records[2].Message, "Nested");
	EXPECT_TRUE(std::ranges::is_sorted(Records, {}, &Durin::FLogRecord::Sequence));
}

TEST(FLoggerTests, ListenerCanRequestShutdownWithoutDeadlock)
{
	Durin::FLogger Logger;
	ASSERT_TRUE(Logger.Initialize(MakeSettings(MakeTestDirectory("CallbackShutdown"))));
	std::promise<void> ShutdownReturned;
	Logger.AddListener([&](const Durin::FLogRecord&) {
		Logger.Shutdown();
		ShutdownReturned.set_value();
	});
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Stop from callback");
	EXPECT_EQ(ShutdownReturned.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
	Logger.Shutdown(); // Joins the dispatcher when shutdown was initiated by its callback.
}

TEST(FLoggerTests, ReportsDroppedLowPriorityRecordsAfterQueueRecovers)
{
	Durin::FLogger Logger;
	Durin::FLogSettings Settings = MakeSettings(MakeTestDirectory("Overflow"));
	Settings.QueueCapacity = 256;
	ASSERT_TRUE(Logger.Initialize(Settings));
	std::mutex Mutex;
	std::condition_variable Entered;
	std::condition_variable Release;
	bool bEntered = false;
	bool bRelease = false;
	std::atomic<bool> bSawSummary = false;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		if (Record.Message.starts_with("Dropped ")) bSawSummary.store(true, std::memory_order_relaxed);
		if (Record.Message != "Block") return;
		std::unique_lock Lock(Mutex);
		bEntered = true;
		Entered.notify_one();
		Release.wait(Lock, [&] { return bRelease; });
	});
	Logger.Log(Durin::ELogLevel::Trace, std::source_location::current(), "LoggerTests", "Block");
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Entered.wait_for(Lock, std::chrono::seconds(2), [&] { return bEntered; }));
	}
	for (int Index = 0; Index < 600; ++Index)
	{
		Logger.Log(Durin::ELogLevel::Trace, std::source_location::current(), "LoggerTests", "Overflow {}", Index);
	}
	{
		std::scoped_lock Lock(Mutex);
		bRelease = true;
	}
	Release.notify_one();
	Logger.Flush();
	Logger.RemoveListener(Handle);
	EXPECT_TRUE(bSawSummary.load(std::memory_order_relaxed));
}

TEST(FLoggerTests, ReplaysBootstrapRecordsAndSupportsIdempotentLifecycle)
{
	Durin::FLogger Logger;
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "Bootstrap", "Before initialize");
	std::atomic<int32_t> Count = 0;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		if (Record.Module == "Bootstrap") Count.fetch_add(1, std::memory_order_relaxed);
	});
	const Durin::FLogSettings Settings = MakeSettings(MakeTestDirectory("Bootstrap"));
	ASSERT_TRUE(Logger.Initialize(Settings));
	ASSERT_TRUE(Logger.Initialize(Settings));
	Logger.Flush();
	EXPECT_EQ(Count.load(std::memory_order_relaxed), 1);
	Logger.RemoveListener(Handle);
	Logger.Shutdown();
	Logger.Shutdown();
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "Bootstrap", "After shutdown");
}

TEST(FLoggerTests, ReportsBootstrapOverflowInStructuredHistory)
{
	Durin::FLogger Logger;
	testing::internal::CaptureStderr();
	for (int Index = 0; Index < 5001; ++Index)
	{
		Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "Bootstrap", "Overflow {}", Index);
	}
	(void)testing::internal::GetCapturedStderr();
	Durin::FLogSettings Settings = MakeSettings(MakeTestDirectory("BootstrapOverflow"));
	Settings.HistoryCapacity = 65536;
	ASSERT_TRUE(Logger.Initialize(Settings));
	Logger.Flush();

	const Durin::FLogReadResult Read = Logger.ReadRecords(1, 4096);
	ASSERT_EQ(Read.EvictedRecordCount, 1u);
	ASSERT_EQ(Read.Records.size(), 4096u);
	std::vector<Durin::FLogRecord> Records = Read.Records;
	const std::vector<Durin::FLogRecord> Remaining = ReadAll(Logger, Read.NextSequence, 4096);
	Records.insert(Records.end(), Remaining.begin(), Remaining.end());
	ASSERT_EQ(Records.size(), 5001u);
	EXPECT_EQ(Records.front().Sequence, 2u);
	EXPECT_EQ(Records.back().Sequence, 5002u);
	EXPECT_NE(Records.back().Message.find("Discarded 1 bootstrap log records"), std::string::npos);
}

TEST(FLoggerTests, ClearsHistoryAndRestartsSequenceAcrossLifecycleSessions)
{
	Durin::FLogger Logger;
	const Durin::FLogSettings Settings = MakeSettings(MakeTestDirectory("HistoryLifecycle"));
	ASSERT_TRUE(Logger.Initialize(Settings));
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "First session");
	Logger.Flush();
	ASSERT_EQ(Logger.ReadRecords(1).Records.size(), 1u);
	Logger.Shutdown();
	EXPECT_TRUE(Logger.ReadRecords(1).Records.empty());

	ASSERT_TRUE(Logger.Initialize(Settings));
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Second session");
	Logger.Flush();
	const Durin::FLogReadResult Read = Logger.ReadRecords(1);
	ASSERT_EQ(Read.Records.size(), 1u);
	EXPECT_EQ(Read.Records.front().Sequence, 1u);
	EXPECT_EQ(Read.Records.front().Message, "Second session");
}

TEST(FLoggerTests, RotatesFilesAndCleansOldSessions)
{
	const std::filesystem::path Directory = MakeTestDirectory("Rotation");
	for (int Index = 0; Index < 3; ++Index)
	{
		const std::filesystem::path OldFile = Directory / std::format("Durin-CoreTests-2020010{}-000000-1.log", Index + 1);
		std::ofstream(OldFile) << "old";
		std::filesystem::last_write_time(OldFile, std::filesystem::file_time_type::clock::now() - std::chrono::hours(3 - Index));
	}
	Durin::FLogger Logger;
	Durin::FLogSettings Settings = MakeSettings(Directory);
	Settings.MaxFileSizeBytes = 1024;
	Settings.MaxFilesPerSession = 2;
	Settings.MaxSessions = 2;
	ASSERT_TRUE(Logger.Initialize(Settings));
	const std::string Payload(700, 'x');
	for (int Index = 0; Index < 8; ++Index)
	{
		Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "{} {}", Index, Payload);
	}
	Logger.Shutdown();
	const std::vector<std::filesystem::path> Files = FindLogFiles(Directory);
	EXPECT_GE(Files.size(), 2u);
	EXPECT_LE(Files.size(), 3u); // One old session plus two segments for this session.
}

TEST(FLoggerTests, FileInitializationFailureFallsBackWithoutFailingLogger)
{
	const std::filesystem::path Directory = MakeTestDirectory("Fallback");
	const std::filesystem::path NotDirectory = Directory / "file";
	std::ofstream(NotDirectory) << "not a directory";
	Durin::FLogger Logger;
	Durin::FLogSettings Settings = MakeSettings(NotDirectory);
	EXPECT_TRUE(Logger.Initialize(Settings));
	Logger.Log(Durin::ELogLevel::Error, std::source_location::current(), "LoggerTests", "Fallback remains alive");
	Logger.Shutdown();
}

TEST(FLoggerTests, ParsesLogLevelsCaseInsensitivelyWithAliases)
{
	EXPECT_EQ(Durin::StringToLogLevel("trace"), Durin::ELogLevel::Trace);
	EXPECT_EQ(Durin::StringToLogLevel("DEBUG"), Durin::ELogLevel::Debug);
	EXPECT_EQ(Durin::StringToLogLevel("Warning"), Durin::ELogLevel::Warn);
	EXPECT_EQ(Durin::StringToLogLevel("critical"), Durin::ELogLevel::Fatal);
	EXPECT_EQ(Durin::StringToLogLevel("invalid", Durin::ELogLevel::Info), Durin::ELogLevel::Info);
}
