#include "NativeTestSupport.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
	struct FChildResult
	{
		int Status = 0;
		std::filesystem::path CrashDirectory;
	};

	auto ReadText(const std::filesystem::path& Path) -> std::string
	{
		std::ifstream Stream(Path, std::ios::binary);
		return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	}

	auto FindCompleteCrash(const std::filesystem::path& Saved) -> std::filesystem::path
	{
		const std::filesystem::path Root = Saved / "Crashes";
		if (!std::filesystem::is_directory(Root)) return {};
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Root))
		{
			if (Entry.is_directory()
				&& std::filesystem::is_regular_file(Entry.path() / "Complete.marker"))
				return Entry.path();
		}
		return {};
	}

	auto RunCrashChild(
		std::string_view Fixture,
		std::string_view ExtraArgument = {},
		bool bBlockCrashRoot = false) -> FChildResult
	{
		const std::filesystem::path Saved = Durin::Testing::CreateTestFixtureDirectory(
			std::format("MacOSCrash-{}", Fixture));
		if (bBlockCrashRoot)
		{
			std::ofstream BlockedRoot(Saved / "Crashes", std::ios::binary);
			BlockedRoot << "not a directory";
		}

		std::vector<std::string> Storage = {
			DURIN_CRASH_FIXTURE_EXECUTABLE,
			std::format("--native-crash-saved={}", Saved.string()),
			std::format("--native-crash-fixture={}", Fixture)};
		if (!ExtraArgument.empty()) Storage.emplace_back(ExtraArgument);
		std::vector<char*> Arguments;
		for (std::string& Value : Storage) Arguments.push_back(Value.data());
		Arguments.push_back(nullptr);

		pid_t Child = 0;
		EXPECT_EQ(posix_spawn(&Child, Storage.front().c_str(), nullptr, nullptr,
			Arguments.data(), environ), 0);
		if (Child == 0) return {};
		int Status = 0;
		bool bExited = false;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			const pid_t Result = waitpid(Child, &Status, WNOHANG);
			if (Result == Child)
			{
				bExited = true;
				break;
			}
			if (Result < 0 && errno != EINTR) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (!bExited)
		{
			kill(Child, SIGKILL);
			waitpid(Child, &Status, 0);
		}
		EXPECT_TRUE(bExited);
		return {.Status = Status, .CrashDirectory = FindCompleteCrash(Saved)};
	}

	auto ReadContext(const std::filesystem::path& CrashDirectory) -> std::string
	{
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(CrashDirectory))
			if (Entry.path().filename().string().find("CrashContext-v1.txt") != std::string::npos)
				return ReadText(Entry.path());
		return {};
	}

	void ExpectSignalCrash(std::string_view Fixture)
	{
		const FChildResult Result = RunCrashChild(Fixture);
		ASSERT_TRUE(WIFSIGNALED(Result.Status));
		EXPECT_TRUE(WTERMSIG(Result.Status) == SIGSEGV || WTERMSIG(Result.Status) == SIGBUS);
		ASSERT_FALSE(Result.CrashDirectory.empty());
		const std::string Context = ReadContext(Result.CrashDirectory);
		EXPECT_NE(Context.find("ReasonKind=POSIXSignal"), std::string::npos);
		EXPECT_NE(Context.find("DumpResult=SystemManaged"), std::string::npos);
	}
}

TEST(FMacOSNativeCrashCharacterizationTests, CapturesReadWriteAndExecuteFaults)
{
	ExpectSignalCrash("access-read");
	ExpectSignalCrash("access-write");
	ExpectSignalCrash("access-execute");
}

TEST(FMacOSNativeCrashCharacterizationTests, CapturesWorkerAndTerminateFaults)
{
	ExpectSignalCrash("worker-access-read");
	const FChildResult Terminate = RunCrashChild("terminate");
	ASSERT_TRUE(WIFSIGNALED(Terminate.Status));
	EXPECT_EQ(WTERMSIG(Terminate.Status), SIGABRT);
	EXPECT_FALSE(Terminate.CrashDirectory.empty());
}

TEST(FMacOSNativeCrashCharacterizationTests, HonorsDumpCollisionAndWriterFailureOptions)
{
	const FChildResult Disabled = RunCrashChild("access-read", "--native-crash-disable-dump");
	ASSERT_FALSE(Disabled.CrashDirectory.empty());
	EXPECT_NE(ReadContext(Disabled.CrashDirectory).find("DumpResult=Disabled"), std::string::npos);

	const FChildResult Collision = RunCrashChild("access-read", "--native-crash-force-collision");
	ASSERT_FALSE(Collision.CrashDirectory.empty());
	EXPECT_TRUE(Collision.CrashDirectory.filename().string().ends_with("-1"));

	const FChildResult FailedWriter = RunCrashChild("access-read", "--native-crash-fault-writer");
	EXPECT_TRUE(WIFSIGNALED(FailedWriter.Status));
	EXPECT_TRUE(FailedWriter.CrashDirectory.empty());
}

TEST(FMacOSNativeCrashCharacterizationTests, UnwritableRootPreservesNativeSignal)
{
	const FChildResult Result = RunCrashChild("access-read", {}, true);
	EXPECT_TRUE(WIFSIGNALED(Result.Status));
	EXPECT_TRUE(Result.CrashDirectory.empty());
}
