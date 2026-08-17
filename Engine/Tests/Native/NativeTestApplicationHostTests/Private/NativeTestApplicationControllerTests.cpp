#include "NativeTestApplicationProtocol.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

namespace
{
	using namespace Durin::Testing::ApplicationHost;

	// Owns one controller child and exposes its ordinary or signal-derived result.
	class FControllerProcess
	{
	public:
		static auto ControlRootForMode(std::string_view Mode) -> std::filesystem::path
		{
			return Durin::Testing::GetTestWorkDirectory()
				/ "NativeTestApplicationController"
				/ ("run-p" + std::to_string(getpid()) + "-" + std::string(Mode));
		}

		explicit FControllerProcess(std::string_view Mode)
		{
			ControlRoot = ControlRootForMode(Mode);
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

		~FControllerProcess()
		{
			if (Process <= 1) return;
			kill(Process, SIGKILL);
			while (waitpid(Process, nullptr, 0) < 0 && errno == EINTR) {}
		}

		auto Wait() -> int
		{
			int Status = 0;
			while (waitpid(Process, &Status, 0) < 0 && errno == EINTR) {}
			Process = 0;
			return Status;
		}

		auto Terminate() const -> void
		{
			kill(Process, SIGTERM);
		}

		auto Kill() const -> void
		{
			kill(Process, SIGKILL);
		}

		auto WaitForControlRoot() const -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(3);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				std::error_code Error;
				if (std::filesystem::is_directory(ControlRoot, Error)) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		}

		auto WaitForPublishedPid(std::string_view FileName) const -> int
		{
			const auto Deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(3);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				std::error_code Error;
				for (const auto& Entry :
					std::filesystem::recursive_directory_iterator(ControlRoot, Error))
				{
					if (Entry.path().filename() != FileName) continue;
					int Pid = 0;
					std::string ReadError;
					if (ReadPid(Entry.path(), Pid, ReadError)) return Pid;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return 0;
		}

		pid_t Process = 0;
		int SpawnResult = 0;
		std::filesystem::path ControlRoot;
	};

	auto ExitCode(int Status) -> int
	{
		return WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
	}

	auto WaitForExit(int Pid, std::chrono::seconds Timeout) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			if (Pid <= 1 || (kill(Pid, 0) != 0 && errno == ESRCH)) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		return false;
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
	const int Child = Hanging.WaitForPublishedPid(ChildPidFile);
	ASSERT_GT(Child, 1);
	Hanging.Terminate();
	const auto Started = std::chrono::steady_clock::now();
	EXPECT_EQ(ExitCode(Hanging.Wait()), 128 + SIGTERM);
	EXPECT_LT(std::chrono::steady_clock::now() - Started, std::chrono::seconds(7));
	EXPECT_TRUE(WaitForExit(Child, std::chrono::seconds(1)));
}

TEST(NativeTestApplicationController, InterruptionBeforeChildPublicationIsBounded)
{
	const std::filesystem::path ControlRoot =
		FControllerProcess::ControlRootForMode("early-cancel");
	std::error_code Error;
	Durin::Testing::RemoveTestWorkDirectory(ControlRoot, Error);
	ASSERT_FALSE(Error);
	FControllerProcess Early("early-cancel");
	ASSERT_EQ(Early.SpawnResult, 0);
	ASSERT_TRUE(Early.WaitForControlRoot());
	Early.Terminate();
	const auto Started = std::chrono::steady_clock::now();
	EXPECT_EQ(ExitCode(Early.Wait()), 128 + SIGTERM);
	EXPECT_LT(std::chrono::steady_clock::now() - Started, std::chrono::seconds(7));
}

TEST(NativeTestApplicationController, ControllerDisappearanceTerminatesPublishedChild)
{
	FControllerProcess Hanging("hang");
	ASSERT_EQ(Hanging.SpawnResult, 0);
	const int Child = Hanging.WaitForPublishedPid(ChildPidFile);
	const int Host = Hanging.WaitForPublishedPid(HostPidFile);
	ASSERT_GT(Child, 1);
	ASSERT_GT(Host, 1);
	Hanging.Kill();
	const int ControllerStatus = Hanging.Wait();
	EXPECT_TRUE(WIFSIGNALED(ControllerStatus));
	EXPECT_TRUE(WaitForExit(Child, std::chrono::seconds(7)));
	EXPECT_TRUE(WaitForExit(Host, std::chrono::seconds(1)));
}

TEST(NativeTestApplicationController, CancellationEscalatesForTermIgnoringChild)
{
	FControllerProcess Hanging("ignore-term");
	ASSERT_EQ(Hanging.SpawnResult, 0);
	const int Child = Hanging.WaitForPublishedPid(ChildPidFile);
	ASSERT_GT(Child, 1);
	Hanging.Terminate();
	const auto Started = std::chrono::steady_clock::now();
	EXPECT_EQ(ExitCode(Hanging.Wait()), 128 + SIGTERM);
	EXPECT_LT(std::chrono::steady_clock::now() - Started, std::chrono::seconds(7));
	EXPECT_TRUE(WaitForExit(Child, std::chrono::seconds(1)));
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

TEST(NativeTestApplicationController, FailedInvocationRetentionIsBounded)
{
	const std::filesystem::path ControlRoot =
		FControllerProcess::ControlRootForMode("fail");
	std::error_code Error;
	Durin::Testing::RemoveTestWorkDirectory(ControlRoot, Error);
	ASSERT_FALSE(Error);
	for (int Index = 0; Index < 36; ++Index)
	{
		FControllerProcess Failing("fail");
		ASSERT_EQ(Failing.SpawnResult, 0);
		EXPECT_EQ(ExitCode(Failing.Wait()), 7);
	}
	size_t RetainedCount = 0;
	for (const auto& Entry : std::filesystem::directory_iterator(ControlRoot, Error))
	{
		if (Entry.is_directory()
			&& Entry.path().filename().string().starts_with("run-p"))
		{
			++RetainedCount;
		}
	}
	EXPECT_LE(RetainedCount, 32u);
}
