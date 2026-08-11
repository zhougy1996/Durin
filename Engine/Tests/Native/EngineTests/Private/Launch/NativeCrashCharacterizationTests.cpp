#include "Misc/StringConvert.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	struct FChildResult
	{
		DWORD ExitCode = 0;
		std::filesystem::path CrashDirectory;
	};

	auto ReadText(const std::filesystem::path& Path) -> std::string
	{
		std::ifstream Stream(Path, std::ios::binary);
		return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	}

	auto RunCrashChild(std::string_view Fixture, std::string_view ExtraArgument = {}) -> FChildResult
	{
		const std::filesystem::path Saved = Durin::Testing::GetTestWorkDirectory() / Fixture;
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Saved, Error);
		std::filesystem::create_directories(Saved);
		const std::string Command = std::format(
			"\"{}\" --native-crash-saved=\"{}\" --native-crash-fixture={} {}",
			DURIN_CRASH_FIXTURE_EXECUTABLE, Saved.string(), Fixture, ExtraArgument);
		std::wstring WideCommand = Durin::StringUtils::Utf8ToWide(Command);
		STARTUPINFOW StartupInfo{};
		StartupInfo.cb = sizeof(StartupInfo);
		PROCESS_INFORMATION ProcessInfo{};
		EXPECT_TRUE(CreateProcessW(nullptr, WideCommand.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &StartupInfo, &ProcessInfo));
		if (ProcessInfo.hProcess == nullptr) return {};
		CloseHandle(ProcessInfo.hThread);
		const DWORD Wait = WaitForSingleObject(ProcessInfo.hProcess, 15000);
		EXPECT_EQ(Wait, WAIT_OBJECT_0);
		DWORD ExitCode = 0;
		GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
		CloseHandle(ProcessInfo.hProcess);

		std::vector<std::filesystem::path> Directories;
		if (std::filesystem::is_directory(Saved / "Crashes"))
		{
			for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Saved / "Crashes"))
			{
				if (Entry.is_directory()) Directories.push_back(Entry.path());
			}
		}
		EXPECT_LE(Directories.size(), 1u);
		return {.ExitCode = ExitCode, .CrashDirectory = Directories.empty() ? std::filesystem::path{} : Directories.front()};
	}

	void ExpectCompleteAccessViolation(std::string_view Fixture, std::string_view Operation)
	{
		const FChildResult Result = RunCrashChild(Fixture);
		EXPECT_EQ(Result.ExitCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
		ASSERT_FALSE(Result.CrashDirectory.empty());
		EXPECT_TRUE(std::filesystem::is_regular_file(Result.CrashDirectory / "Complete.marker"));
		std::vector<std::filesystem::path> Contexts;
		std::vector<std::filesystem::path> Dumps;
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Result.CrashDirectory))
		{
			if (Entry.path().filename().string().find("CrashContext-v1.txt") != std::string::npos) Contexts.push_back(Entry.path());
			if (Entry.path().extension() == ".dmp") Dumps.push_back(Entry.path());
		}
		ASSERT_EQ(Contexts.size(), 1u);
		ASSERT_EQ(Dumps.size(), 1u);
		EXPECT_GT(std::filesystem::file_size(Dumps.front()), 0u);
		const std::string Context = ReadText(Contexts.front());
		EXPECT_NE(Context.find("ReasonCode=0xc0000005"), std::string::npos);
		EXPECT_NE(Context.find(std::format("AccessViolationOperation={}", Operation)), std::string::npos);
		EXPECT_NE(Context.find("DumpResult=Written"), std::string::npos);
	}
}

TEST(FNativeCrashCharacterizationTests, CapturesReadWriteAndExecuteAccessViolations)
{
	ExpectCompleteAccessViolation("access-read", "Read");
	ExpectCompleteAccessViolation("access-write", "Write");
	ExpectCompleteAccessViolation("access-execute", "Execute");
}

TEST(FNativeCrashCharacterizationTests, CapturesWorkerThreadAndTerminatePaths)
{
	ExpectCompleteAccessViolation("worker-access-read", "Read");
	const FChildResult Terminate = RunCrashChild("terminate");
	EXPECT_EQ(Terminate.ExitCode, 0xE0000001u);
	ASSERT_FALSE(Terminate.CrashDirectory.empty());
	EXPECT_TRUE(std::filesystem::is_regular_file(Terminate.CrashDirectory / "Complete.marker"));
}

TEST(FNativeCrashCharacterizationTests, DumpFailureRetainsContextAndOriginalStatus)
{
	const FChildResult Result = RunCrashChild("access-read", "--native-crash-disable-dump");
	EXPECT_EQ(Result.ExitCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
	ASSERT_FALSE(Result.CrashDirectory.empty());
	std::vector<std::filesystem::path> Contexts;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Result.CrashDirectory))
	{
		if (Entry.path().filename().string().find("CrashContext-v1.txt") != std::string::npos) Contexts.push_back(Entry.path());
		EXPECT_NE(Entry.path().extension(), ".dmp");
	}
	ASSERT_EQ(Contexts.size(), 1u);
	const std::string Context = ReadText(Contexts.front());
	EXPECT_NE(Context.find("DumpResult=Failed"), std::string::npos);
	EXPECT_NE(Context.find("DumpError=126"), std::string::npos);
	EXPECT_TRUE(std::filesystem::is_regular_file(Result.CrashDirectory / "Complete.marker"));
}

TEST(FNativeCrashCharacterizationTests, SimultaneousFaultsTerminateWithoutASecondArtifactSet)
{
	const FChildResult Result = RunCrashChild("simultaneous-access");
	EXPECT_EQ(Result.ExitCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
	if (Result.CrashDirectory.empty()) return;
	size_t ContextCount = 0;
	size_t DumpCount = 0;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Result.CrashDirectory))
	{
		if (Entry.path().filename().string().find("CrashContext-v1.txt") != std::string::npos) ++ContextCount;
		if (Entry.path().extension() == ".dmp") ++DumpCount;
	}
	EXPECT_LE(ContextCount, 1u);
	EXPECT_LE(DumpCount, 1u);
}

TEST(FNativeCrashCharacterizationTests, CharacterizesStackOverflowWithReservedHandlerStack)
{
	const FChildResult Result = RunCrashChild("stack-overflow");
	EXPECT_TRUE(Result.ExitCode == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW)
		|| Result.ExitCode == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
	if (!Result.CrashDirectory.empty())
		EXPECT_TRUE(std::filesystem::is_regular_file(Result.CrashDirectory / "Complete.marker"));
}
