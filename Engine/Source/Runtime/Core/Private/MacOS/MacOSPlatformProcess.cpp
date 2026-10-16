#include "MacOS/MacOSPlatformProcess.h"
#include "HAL/ProcessLaunch.h"
#include <crt_externs.h>
#include <climits>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>


namespace Durin
{
	namespace
	{
		auto FormatErrno(int Error) -> std::string
		{
			return std::format("macOS error {}: {}", Error,
				std::generic_category().message(Error));
		}

		struct FSpawnFileActions
		{
			posix_spawn_file_actions_t Value{};
			bool bInitialized = false;
			~FSpawnFileActions() { if (bInitialized) posix_spawn_file_actions_destroy(&Value); }
		};

		auto Spawn(std::string_view Executable, const FProcessLaunchOptions& Options)
			-> std::expected<pid_t, FPlatformProcessError>
		{
			if (const auto Valid = Detail::ValidateProcessLaunch(Executable, Options); !Valid)
				return std::unexpected(Valid.error());
			const auto Failure = [&](int Error) {
				return FPlatformProcessError{EPlatformProcessError::Launch,
					std::format("Could not launch executable: {}.", FormatErrno(Error)), std::string(Executable), Error};
			};
			std::string ExecutableStorage(Executable);
			auto Arguments = Options.Arguments;
			std::vector<char*> ArgumentPointers;
			ArgumentPointers.reserve(Arguments.size() + 2);
			ArgumentPointers.push_back(ExecutableStorage.data());
			for (auto& Argument : Arguments) ArgumentPointers.push_back(Argument.data());
			ArgumentPointers.push_back(nullptr);

			std::vector<std::string> EnvironmentStorage;
			std::vector<char*> EnvironmentPointers;
			char** Environment = *_NSGetEnviron();
			if (!Options.EnvironmentOverrides.empty())
			{
				std::map<std::string, std::string> Values;
				for (char** Entry = Environment; Entry && *Entry; ++Entry)
				{
					const std::string_view Text(*Entry);
					const auto Separator = Text.find('=');
					if (Separator != Text.npos)
						Values[std::string(Text.substr(0, Separator))] = Text.substr(Separator + 1);
				}
				for (const auto& [Name, Value] : Options.EnvironmentOverrides)
				{
					if (Value) Values[Name] = *Value;
					else Values.erase(Name);
				}
				for (const auto& [Name, Value] : Values) EnvironmentStorage.push_back(Name + "=" + Value);
				for (auto& Entry : EnvironmentStorage) EnvironmentPointers.push_back(Entry.data());
				EnvironmentPointers.push_back(nullptr);
				Environment = EnvironmentPointers.data();
			}

			FSpawnFileActions Actions;
			if (!Options.WorkingDirectory.empty())
			{
				const int Initialized = posix_spawn_file_actions_init(&Actions.Value);
				if (Initialized != 0) return std::unexpected(Failure(Initialized));
				Actions.bInitialized = true;
				// Child-only chdir; the parent directory never changes, even on failure.
				const int DirectoryResult = posix_spawn_file_actions_addchdir_np(&Actions.Value,
					Options.WorkingDirectory.c_str());
				if (DirectoryResult != 0) return std::unexpected(Failure(DirectoryResult));
			}
			pid_t ChildProcess = 0;
			const int SpawnResult = posix_spawn(&ChildProcess, ExecutableStorage.c_str(),
				Actions.bInitialized ? &Actions.Value : nullptr, nullptr, ArgumentPointers.data(), Environment);
			if (SpawnResult != 0) return std::unexpected(Failure(SpawnResult));
			return ChildProcess;
		}

		auto WaitForChild(pid_t ChildProcess, int32& OutReturnCode, std::string* OutError) -> bool
		{
			int Status = 0;
			pid_t Result = 0;
			do
			{
				Result = waitpid(ChildProcess, &Status, 0);
			} while (Result == -1 && errno == EINTR);
			if (Result == -1)
			{
				if (OutError) *OutError = std::format(
					"Could not wait for child process {}: {}.", ChildProcess, FormatErrno(errno));
				return false;
			}
			if (WIFEXITED(Status))
			{
				OutReturnCode = WEXITSTATUS(Status);
				return true;
			}
			if (WIFSIGNALED(Status))
			{
				OutReturnCode = 128 + WTERMSIG(Status);
				return true;
			}
			if (OutError) *OutError = std::format(
				"Child process {} ended with unsupported wait status {}.", ChildProcess, Status);
			return false;
		}
	}

	auto FMacOSPlatformProcess::ExecutablePath() -> const char*
	{
		static const std::string Path = [] {
			uint32_t Size = 0;
			(void)_NSGetExecutablePath(nullptr, &Size);
			std::vector<char> Buffer(Size);
			if (_NSGetExecutablePath(Buffer.data(), &Size) != 0) return std::string{};

			char* ResolvedPath = realpath(Buffer.data(), nullptr);
			if (!ResolvedPath) return std::string(Buffer.data());
			std::string Result(ResolvedPath);
			std::free(ResolvedPath);
			return Result;
		}();
		return Path.c_str();
	}

	auto FMacOSPlatformProcess::CurrentProcessId() -> uint32
	{
		return static_cast<uint32>(getpid());
	}

	auto FMacOSPlatformProcess::WaitForProcessExit(
		uint32 ProcessId) -> std::expected<void, FPlatformProcessError>
	{
		if (ProcessId > static_cast<uint32>(INT_MAX))
		{
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::InvalidArguments, "Process identifier is outside the macOS pid range."});
		}

		const int Queue = kqueue();
		if (Queue == -1)
		{
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("Could not create process wait queue: {}.", FormatErrno(errno))});
		}

		struct kevent Change{};
		EV_SET(&Change, static_cast<uintptr_t>(ProcessId), EVFILT_PROC,
			EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
		if (kevent(Queue, &Change, 1, nullptr, 0, nullptr) == -1)
		{
			const int Error = errno;
			close(Queue);
			if (Error == ESRCH) return {};
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("Could not observe process {}: {}.",
					ProcessId, FormatErrno(Error))});
		}

		struct kevent Event{};
		int WaitResult = 0;
		do
		{
			WaitResult = kevent(Queue, nullptr, 0, &Event, 1, nullptr);
		} while (WaitResult == -1 && errno == EINTR);
		const int Error = errno;
		close(Queue);
		if (WaitResult == 1) return {};
		return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("Could not wait for process {}: {}.",
				ProcessId, FormatErrno(Error))});
	}

	auto FMacOSPlatformProcess::LaunchProcess(
		std::string_view Executable,
		const FProcessLaunchOptions& Options) -> std::expected<void, FPlatformProcessError>
	{
		const auto Spawned = Spawn(Executable, Options);
		if (!Spawned) return std::unexpected(Spawned.error());
		const pid_t ChildProcess = *Spawned;
		std::thread([ChildProcess] {
			int32 ReturnCode = 0;
			(void)WaitForChild(ChildProcess, ReturnCode, nullptr);
		}).detach();
		return {};
	}

	auto FMacOSPlatformProcess::ExecuteProcess(
		std::string_view Executable,
		const FProcessLaunchOptions& Options) -> std::expected<int32, FPlatformProcessError>
	{
		std::string Error;
		const auto Spawned = Spawn(Executable, Options);
		if (!Spawned) return std::unexpected(Spawned.error());
		const pid_t ChildProcess = *Spawned;
		int32 ReturnCode = 0;
		if (!WaitForChild(ChildProcess, ReturnCode, &Error))
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::move(Error)});
		return ReturnCode;
	}

	auto FMacOSPlatformProcess::OpenPath(
		std::string_view Path) -> std::expected<void, FPlatformProcessError>
	{
		if (Path.empty())
		{
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::InvalidArguments, "Path to open is empty."});
		}
		const auto Result = ExecuteProcess("/usr/bin/open",
			FProcessLaunchOptions{.Arguments = {"--", std::string(Path)}});
		if (!Result) return std::unexpected(Result.error());
		const int32 ReturnCode = *Result;
		if (ReturnCode == 0) return {};
		return std::unexpected(FPlatformProcessError{EPlatformProcessError::OpenPath, std::format(
			"Could not open \"{}\": /usr/bin/open exited with code {}.", Path, ReturnCode), std::string(Path), std::nullopt, ReturnCode});
	}
}
