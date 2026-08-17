#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

namespace
{
	// Owns one controller child and exposes its ordinary or signal-derived result.
	class FControllerProcess
	{
	public:
		explicit FControllerProcess(std::string_view Mode)
		{
			ControlRoot = std::filesystem::path(
				"/private/tmp/DurinNativeTestApplicationControllerTests")
				/ ("run-p" + std::to_string(getpid()) + "-" + std::string(Mode));
			std::vector<std::string> Storage{
				DURIN_APPLICATION_TEST_CONTROLLER,
				"--host-bundle", DURIN_APPLICATION_TEST_HOST_BUNDLE,
				"--control-root", ControlRoot.string(),
				"--artifact-root",
					std::filesystem::path(DURIN_APPLICATION_TEST_PROBE).parent_path().parent_path().string(),
				"--",
				DURIN_APPLICATION_TEST_PROBE, std::string(Mode)};
			std::vector<char*> Arguments;
			for (std::string& Argument : Storage) Arguments.push_back(Argument.data());
			Arguments.push_back(nullptr);
			posix_spawn_file_actions_t Actions;
			posix_spawn_file_actions_init(&Actions);
			posix_spawn_file_actions_addchdir_np(
				&Actions, std::filesystem::path(DURIN_APPLICATION_TEST_PROBE).parent_path().c_str());
			SpawnResult = posix_spawn(&Process, Storage[0].c_str(), &Actions, nullptr,
				Arguments.data(), environ);
			posix_spawn_file_actions_destroy(&Actions);
		}

		auto Wait() -> int
		{
			int Status = 0;
			while (waitpid(Process, &Status, 0) < 0 && errno == EINTR) {}
			return Status;
		}

		auto Terminate() const -> void
		{
			kill(Process, SIGTERM);
		}

		pid_t Process = 0;
		int SpawnResult = 0;
		std::filesystem::path ControlRoot;
	};

	auto ExitCode(int Status) -> int
	{
		return WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
	}
}

TEST(NativeTestApplicationController, PropagatesPassAndOrdinaryFailure)
{
	FControllerProcess Passing("pass");
	ASSERT_EQ(Passing.SpawnResult, 0);
	EXPECT_EQ(ExitCode(Passing.Wait()), 0);

	FControllerProcess Failing("fail");
	ASSERT_EQ(Failing.SpawnResult, 0);
	EXPECT_EQ(ExitCode(Failing.Wait()), 7);
}

TEST(NativeTestApplicationController, MapsSignalCrashAndRetainsEvidence)
{
	FControllerProcess Crashing("crash");
	ASSERT_EQ(Crashing.SpawnResult, 0);
	EXPECT_EQ(ExitCode(Crashing.Wait()), 128 + SIGABRT);
	std::error_code Error;
	EXPECT_TRUE(std::filesystem::exists(Crashing.ControlRoot, Error));
}

TEST(NativeTestApplicationController, CancellationIsBounded)
{
	FControllerProcess Hanging("hang");
	ASSERT_EQ(Hanging.SpawnResult, 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	Hanging.Terminate();
	const auto Started = std::chrono::steady_clock::now();
	EXPECT_EQ(ExitCode(Hanging.Wait()), 128 + SIGTERM);
	EXPECT_LT(std::chrono::steady_clock::now() - Started, std::chrono::seconds(7));
}

TEST(NativeTestApplicationController, ConcurrentInvocationsRemainIsolated)
{
	FControllerProcess First("pass");
	FControllerProcess Second("pass");
	ASSERT_EQ(First.SpawnResult, 0);
	ASSERT_EQ(Second.SpawnResult, 0);
	EXPECT_EQ(ExitCode(First.Wait()), 0);
	EXPECT_EQ(ExitCode(Second.Wait()), 0);
}
