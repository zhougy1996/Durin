#include "Logging/Logger.h"

#include <gtest/gtest.h>

TEST(FLoggerTests, DeliversStructuredRecordsAndUnregistersListeners)
{
	Durin::FLogger& Logger = Durin::FLogger::Get();
	std::mutex Mutex;
	std::vector<Durin::FLogRecord> Records;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord& Record) {
		std::scoped_lock Lock(Mutex);
		Records.push_back(Record);
	});

	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Value {}", 42);
	Logger.RemoveListener(Handle);
	Logger.Log(Durin::ELogLevel::Info, std::source_location::current(), "LoggerTests", "Ignored");

	std::scoped_lock Lock(Mutex);
	ASSERT_EQ(Records.size(), 1u);
	EXPECT_EQ(Records[0].Level, Durin::ELogLevel::Info);
	EXPECT_EQ(Records[0].Module, "LoggerTests");
	EXPECT_EQ(Records[0].Message, "Value 42");
}

TEST(FLoggerTests, SupportsConcurrentProducers)
{
	Durin::FLogger& Logger = Durin::FLogger::Get();
	std::atomic<int32_t> RecordCount = 0;
	const Durin::FLogListenerHandle Handle = Logger.AddListener([&](const Durin::FLogRecord&) {
		RecordCount.fetch_add(1, std::memory_order_relaxed);
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
	for (std::thread& Thread : Threads)
	{
		Thread.join();
	}
	Logger.RemoveListener(Handle);

	EXPECT_EQ(RecordCount.load(std::memory_order_relaxed), 100);
}
