#include "Panels/ConsoleRecordModel.h"

#include <gtest/gtest.h>

TEST(FConsoleRecordModelTests, BoundsCombinedLogAndCommandRecords)
{
	Durin::Editor::Level::FConsoleRecordModel Model;
	for (size_t Index = 0; Index < Durin::Editor::Level::FConsoleRecordModel::MaxRecords; ++Index)
	{
		Durin::FLogRecord Record;
		Record.Sequence = Index + 1;
		Record.Level = Durin::ELogLevel::Info;
		Record.Message = std::format("Log {}", Index);
		Model.AddLog(std::move(Record));
	}
	Model.AddText(Durin::Editor::Level::EConsoleRecordType::Command, "> help");
	Model.AddText(Durin::Editor::Level::EConsoleRecordType::Result, "Available commands");

	const std::deque<Durin::Editor::Level::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), Durin::Editor::Level::FConsoleRecordModel::MaxRecords);
	EXPECT_EQ(Records.front().Log.Sequence, 3u);
	EXPECT_EQ(Records[Records.size() - 2].Type, Durin::Editor::Level::EConsoleRecordType::Command);
	EXPECT_EQ(Records.back().Type, Durin::Editor::Level::EConsoleRecordType::Result);
}

TEST(FConsoleRecordModelTests, RepresentsHistoryGapsAndFatalLogs)
{
	Durin::Editor::Level::FConsoleRecordModel Model;
	Durin::FLogRecord FatalRecord;
	FatalRecord.Sequence = 38;
	FatalRecord.Level = Durin::ELogLevel::Fatal;
	FatalRecord.Module = "Engine";
	FatalRecord.CategoryOverride = "Runtime";
	FatalRecord.Message = "Fatal failure";
	Model.AddLog(std::move(FatalRecord));
	Model.SetHistoryGap(37);

	const std::deque<Durin::Editor::Level::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), 2u);
	EXPECT_EQ(Records.front().Type, Durin::Editor::Level::EConsoleRecordType::Log);
	EXPECT_EQ(Records.front().Log.Level, Durin::ELogLevel::Fatal);
	EXPECT_EQ(Records.front().Log.Module, "Engine");
	EXPECT_EQ(Records.front().Log.CategoryOverride, "Runtime");
	EXPECT_EQ(Records.front().Log.GetCategory(), "Runtime");
	EXPECT_EQ(Records.back().Type, Durin::Editor::Level::EConsoleRecordType::HistoryGap);
	EXPECT_NE(Records.back().Text.find("37"), std::string::npos);
	EXPECT_EQ(Durin::Editor::Level::FConsoleRecordModel::LogLevelCount, 6u);
}

TEST(FConsoleRecordModelTests, SplitsMultilineTextIntoSingleLineRecords)
{
	Durin::Editor::Level::FConsoleRecordModel Model;
	Model.AddText(Durin::Editor::Level::EConsoleRecordType::Result, "Available commands:\r\n  help\n  pie.play");

	const std::deque<Durin::Editor::Level::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), 3u);
	EXPECT_EQ(Records[0].Type, Durin::Editor::Level::EConsoleRecordType::Result);
	EXPECT_EQ(Records[0].Text, "Available commands:");
	EXPECT_EQ(Records[1].Text, "  help");
	EXPECT_EQ(Records[2].Text, "  pie.play");
}

TEST(FConsoleRecordModelTests, KeepsHistoryGapVisibleAtCapacity)
{
	Durin::Editor::Level::FConsoleRecordModel Model;
	Model.SetHistoryGap(9);
	for (size_t Index = 0; Index < Durin::Editor::Level::FConsoleRecordModel::MaxRecords; ++Index)
	{
		Durin::FLogRecord Record;
		Record.Sequence = Index + 10;
		Model.AddLog(std::move(Record));
	}
	Model.SetHistoryGap(9);

	const std::deque<Durin::Editor::Level::FConsoleRecord>& Records = Model.GetRecords();
	ASSERT_EQ(Records.size(), Durin::Editor::Level::FConsoleRecordModel::MaxRecords);
	EXPECT_EQ(Records.front().Log.Sequence, 11u);
	EXPECT_EQ(Records.back().Type, Durin::Editor::Level::EConsoleRecordType::HistoryGap);
}
