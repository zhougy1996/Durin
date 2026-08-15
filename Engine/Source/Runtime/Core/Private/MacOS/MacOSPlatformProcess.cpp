#include "MacOS/MacOSPlatformProcess.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace Durin
{
	namespace
	{
		auto FormatErrno(int Error) -> std::string
		{
			return std::format("macOS error {}: {}", Error,
				std::generic_category().message(Error));
		}

		auto ParseArguments(
			std::string_view Arguments,
			std::vector<std::string>& OutArguments,
			std::string* OutError) -> bool
		{
			std::string Current;
			char Quote = '\0';
			bool Escaped = false;
			bool HasToken = false;

			for (const char Character : Arguments)
			{
				if (Escaped)
				{
					Current.push_back(Character);
					Escaped = false;
					HasToken = true;
					continue;
				}
				if (Character == '\\' && Quote != '\'')
				{
					Escaped = true;
					HasToken = true;
					continue;
				}
				if ((Character == '\'' || Character == '"')
					&& (Quote == '\0' || Quote == Character))
				{
					Quote = Quote == '\0' ? Character : '\0';
					HasToken = true;
					continue;
				}
				if (std::isspace(static_cast<unsigned char>(Character))
					&& Quote == '\0')
				{
					if (HasToken)
					{
						OutArguments.push_back(std::move(Current));
						Current.clear();
						HasToken = false;
					}
					continue;
				}
				Current.push_back(Character);
				HasToken = true;
			}

			if (Escaped || Quote != '\0')
			{
				if (OutError)
					*OutError = "Process arguments contain an unfinished escape or quote.";
				return false;
			}
			if (HasToken) OutArguments.push_back(std::move(Current));
			return true;
		}

		auto Spawn(
			std::string_view Executable,
			std::vector<std::string> Arguments,
			std::string* OutError) -> bool
		{
			if (Executable.empty())
			{
				if (OutError) *OutError = "Process executable path is empty.";
				return false;
			}

			std::string ExecutableStorage(Executable);
			std::vector<char*> ArgumentPointers;
			ArgumentPointers.reserve(Arguments.size() + 2);
			ArgumentPointers.push_back(ExecutableStorage.data());
			for (std::string& Argument : Arguments)
				ArgumentPointers.push_back(Argument.data());
			ArgumentPointers.push_back(nullptr);

			pid_t ChildProcess = 0;
			const int SpawnResult = posix_spawn(
				&ChildProcess,
				ExecutableStorage.c_str(),
				nullptr,
				nullptr,
				ArgumentPointers.data(),
				environ);
			if (SpawnResult != 0)
			{
				if (OutError)
					*OutError = std::format(
						"Could not launch \"{}\": {}.",
						Executable, FormatErrno(SpawnResult));
				return false;
			}

			std::thread([ChildProcess] {
				int Status = 0;
				while (waitpid(ChildProcess, &Status, 0) == -1 && errno == EINTR)
				{
				}
			}).detach();
			return true;
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
		uint32 ProcessId,
		std::string* OutError) -> bool
	{
		if (ProcessId > static_cast<uint32>(INT_MAX))
		{
			if (OutError) *OutError = "Process identifier is outside the macOS pid range.";
			return false;
		}

		const int Queue = kqueue();
		if (Queue == -1)
		{
			if (OutError) *OutError = std::format("Could not create process wait queue: {}.", FormatErrno(errno));
			return false;
		}

		struct kevent Change{};
		EV_SET(&Change, static_cast<uintptr_t>(ProcessId), EVFILT_PROC,
			EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
		if (kevent(Queue, &Change, 1, nullptr, 0, nullptr) == -1)
		{
			const int Error = errno;
			close(Queue);
			if (Error == ESRCH) return true;
			if (OutError)
				*OutError = std::format("Could not observe process {}: {}.",
					ProcessId, FormatErrno(Error));
			return false;
		}

		struct kevent Event{};
		int WaitResult = 0;
		do
		{
			WaitResult = kevent(Queue, nullptr, 0, &Event, 1, nullptr);
		} while (WaitResult == -1 && errno == EINTR);
		const int Error = errno;
		close(Queue);
		if (WaitResult == 1) return true;
		if (OutError)
			*OutError = std::format("Could not wait for process {}: {}.",
				ProcessId, FormatErrno(Error));
		return false;
	}

	auto FMacOSPlatformProcess::LaunchProcess(
		std::string_view Executable,
		std::string_view Arguments,
		std::string* OutError) -> bool
	{
		std::vector<std::string> ParsedArguments;
		if (!ParseArguments(Arguments, ParsedArguments, OutError)) return false;
		return Spawn(Executable, std::move(ParsedArguments), OutError);
	}

	auto FMacOSPlatformProcess::OpenPath(
		std::string_view Path,
		std::string* OutError) -> bool
	{
		return Spawn("/usr/bin/open", { "--", std::string(Path) }, OutError);
	}
}
