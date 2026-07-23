#include "Panels/ConsoleRecordModel.h"

#include <gtest/gtest.h>

TEST(FConsoleRecordModelTests, BoundsCombinedLogAndCommandRecords)
{
	Durin::FConsoleRecordModel Model;
	for (size_t Index = 0; Index < Durin::FConsoleRecordModel::MaxRecords; ++Index)
	{
		Durin::FLogRecord Record;
		Record.Sequence = Index + 1;
		Record.Level = Durin::ELogLevel::Info;
		Record.Message = std::format("Log {}", Index);
		Model.AddLog(std::move(Record));
	}
	Model.AddText(Durin::EConsoleRecordType::Command, "> help");
	Model.AddText(Durin::EConsoleRecordType::Result, "Available commands");

	const std::deque<Durin::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), Durin::FConsoleRecordModel::MaxRecords);
	EXPECT_EQ(Records.front().Log.Sequence, 3u);
	EXPECT_EQ(Records[Records.size() - 2].Type, Durin::EConsoleRecordType::Command);
	EXPECT_EQ(Records.back().Type, Durin::EConsoleRecordType::Result);
}

TEST(FConsoleRecordModelTests, RepresentsHistoryGapsAndFatalLogs)
{
	Durin::FConsoleRecordModel Model;
	Durin::FLogRecord FatalRecord;
	FatalRecord.Sequence = 38;
	FatalRecord.Level = Durin::ELogLevel::Fatal;
	FatalRecord.Module = "Engine";
	FatalRecord.CategoryOverride = "Runtime";
	FatalRecord.Message = "Fatal failure";
	Model.AddLog(std::move(FatalRecord));
	Model.SetHistoryGap(37);

	const std::deque<Durin::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), 2u);
	EXPECT_EQ(Records.front().Type, Durin::EConsoleRecordType::Log);
	EXPECT_EQ(Records.front().Log.Level, Durin::ELogLevel::Fatal);
	EXPECT_EQ(Records.front().Log.Module, "Engine");
	EXPECT_EQ(Records.front().Log.CategoryOverride, "Runtime");
	EXPECT_EQ(Records.front().Log.GetCategory(), "Runtime");
	EXPECT_EQ(Records.back().Type, Durin::EConsoleRecordType::HistoryGap);
	EXPECT_NE(Records.back().Text.find("37"), std::string::npos);
	EXPECT_EQ(Durin::FConsoleRecordModel::LogLevelCount, 6u);
}

TEST(FConsoleRecordModelTests, KeepsHistoryGapVisibleAtCapacity)
{
	Durin::FConsoleRecordModel Model;
	Model.SetHistoryGap(9);
	for (size_t Index = 0; Index < Durin::FConsoleRecordModel::MaxRecords; ++Index)
	{
		Durin::FLogRecord Record;
		Record.Sequence = Index + 10;
		Model.AddLog(std::move(Record));
	}
	Model.SetHistoryGap(9);

	const std::deque<Durin::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), Durin::FConsoleRecordModel::MaxRecords);
	EXPECT_EQ(Records.front().Log.Sequence, 11u);
	EXPECT_EQ(Records.back().Type, Durin::EConsoleRecordType::HistoryGap);
}
