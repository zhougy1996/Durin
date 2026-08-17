#include "NativeTestApplicationProtocol.h"
#include "NativeTestApplicationProcess.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <spawn.h>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char** environ;

namespace
{
	using namespace Durin::Testing::ApplicationHost;

	constexpr size_t RetainedInvocationLimit = 32;
	volatile sig_atomic_t GTerminationSignal = 0;

	void HandleTermination(int Signal)
	{
		GTerminationSignal = Signal;
	}

	// Separates launcher policy arguments from the exact native-test command.
	struct FControllerArguments
	{
		std::filesystem::path HostBundle;
		std::filesystem::path ControlRoot;
		std::filesystem::path ArtifactRoot;
		std::vector<std::string> TestCommand;
	};

	auto ParseArguments(int ArgumentCount, char** Arguments,
		FControllerArguments& Parsed, std::string& Error) -> bool
	{
		int Index = 1;
		while (Index < ArgumentCount)
		{
			const std::string_view Argument = Arguments[Index++];
			if (Argument == "--") break;
			if (Index >= ArgumentCount)
			{
				Error = "controller option is missing a value";
				return false;
			}
			if (Argument == "--host-bundle") Parsed.HostBundle = Arguments[Index++];
			else if (Argument == "--control-root") Parsed.ControlRoot = Arguments[Index++];
			else if (Argument == "--artifact-root") Parsed.ArtifactRoot = Arguments[Index++];
			else
			{
				Error = "unknown controller option: " + std::string(Argument);
				return false;
			}
		}
		for (; Index < ArgumentCount; ++Index) Parsed.TestCommand.emplace_back(Arguments[Index]);
		if (Parsed.HostBundle.empty() || Parsed.ControlRoot.empty() || Parsed.ArtifactRoot.empty()
			|| Parsed.TestCommand.empty())
		{
			Error = "controller invocation is incomplete";
			return false;
		}
		return true;
	}

	auto MakeNonce() -> std::string
	{
		std::random_device Device;
		std::mt19937_64 Generator(Device());
		const uint64_t First = Generator();
		const uint64_t Second = Generator();
		char Buffer[65]{};
		std::snprintf(Buffer, sizeof(Buffer), "%016llx%016llx",
			static_cast<unsigned long long>(First),
			static_cast<unsigned long long>(Second));
		return Buffer;
	}

	auto CopyFileToDescriptor(const std::filesystem::path& Path, int Destination) -> void
	{
		FScopedFileDescriptor Source(open(Path.c_str(), O_RDONLY | O_NOFOLLOW));
		if (!Source.IsValid()) return;
		char Buffer[16384];
		for (;;)
		{
			const ssize_t Read = read(Source.Get(), Buffer, sizeof(Buffer));
			if (Read < 0 && errno == EINTR) continue;
			if (Read <= 0) break;
			size_t Offset = 0;
			while (Offset < static_cast<size_t>(Read))
			{
				const ssize_t Written = write(Destination, Buffer + Offset,
					static_cast<size_t>(Read) - Offset);
				if (Written < 0 && errno == EINTR) continue;
				if (Written <= 0) break;
				Offset += static_cast<size_t>(Written);
			}
		}
	}

	auto ReadRecordedPid(const std::filesystem::path& Directory,
		std::string_view FileName) -> int
	{
		int Pid = 0;
		std::string Error;
		if (!ReadPid(Directory / FileName, Pid, Error) || Pid == getpid()) return 0;
		return Pid;
	}

	auto TerminateExactProcesses(const std::filesystem::path& Directory) -> void
	{
		const int Child = ReadRecordedPid(Directory, ChildPidFile);
		const int Host = ReadRecordedPid(Directory, HostPidFile);
		if (Child > 1) kill(Child, SIGTERM);
		if (Host > 1) kill(Host, SIGTERM);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			const bool ChildAlive = Child > 1 && kill(Child, 0) == 0;
			const bool HostAlive = Host > 1 && kill(Host, 0) == 0;
			if (!ChildAlive && !HostAlive) return;
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		if (Child > 1 && kill(Child, 0) == 0) kill(Child, SIGKILL);
		if (Host > 1 && kill(Host, 0) == 0) kill(Host, SIGKILL);
	}

	auto PruneRetainedInvocations(const std::filesystem::path& Root,
		const std::filesystem::path& Current) -> void
	{
		std::error_code Error;
		std::vector<std::filesystem::directory_entry> Entries;
		for (const auto& Entry : std::filesystem::directory_iterator(Root, Error))
		{
			if (Entry.is_directory() && Entry.path() != Current
				&& Entry.path().filename().string().starts_with("run-p"))
			{
				Entries.push_back(Entry);
			}
		}
		std::sort(Entries.begin(), Entries.end(), [](const auto& Left, const auto& Right)
		{
			return Left.last_write_time() > Right.last_write_time();
		});
		const size_t RetainedPreviousLimit = RetainedInvocationLimit > 0 ?
			RetainedInvocationLimit - 1 : 0;
		for (size_t Index = RetainedPreviousLimit; Index < Entries.size(); ++Index)
		{
			const int Host = ReadRecordedPid(Entries[Index].path(), HostPidFile);
			if (Host > 1 && kill(Host, 0) == 0) continue;
			std::filesystem::remove_all(Entries[Index].path(), Error);
		}
	}
}

int main(int ArgumentCount, char** Arguments)
{
	using namespace Durin::Testing::ApplicationHost;

	FControllerArguments Parsed;
	std::string Error;
	if (!ParseArguments(ArgumentCount, Arguments, Parsed, Error))
	{
		std::cerr << "Durin native-test application controller: " << Error << '\n';
		return 125;
	}
	std::signal(SIGTERM, HandleTermination);
	std::signal(SIGINT, HandleTermination);
	std::signal(SIGHUP, HandleTermination);

	std::error_code PathError;
	Parsed.HostBundle = std::filesystem::canonical(Parsed.HostBundle, PathError);
	if (PathError || !std::filesystem::is_directory(Parsed.HostBundle))
	{
		std::cerr << "Durin native-test application controller: host bundle is invalid.\n";
		return 125;
	}
	std::filesystem::create_directories(Parsed.ControlRoot, PathError);
	if (PathError || chmod(Parsed.ControlRoot.c_str(), 0700) != 0)
	{
		std::cerr << "Durin native-test application controller: control root is unavailable.\n";
		return 125;
	}
	Parsed.ControlRoot = std::filesystem::canonical(Parsed.ControlRoot, PathError);
	if (PathError)
	{
		std::cerr << "Durin native-test application controller: control root is invalid.\n";
		return 125;
	}
	Parsed.ArtifactRoot = std::filesystem::canonical(Parsed.ArtifactRoot, PathError);
	if (PathError || !std::filesystem::is_directory(Parsed.ArtifactRoot))
	{
		std::cerr << "Durin native-test application controller: artifact root is invalid.\n";
		return 125;
	}

	const std::string Nonce = MakeNonce();
	const std::filesystem::path ControlDirectory = Parsed.ControlRoot /
		("run-p" + std::to_string(getpid()) + "-" + Nonce);
	if (mkdir(ControlDirectory.c_str(), 0700) != 0)
	{
		std::cerr << "Durin native-test application controller: cannot create control directory: "
			<< std::strerror(errno) << '\n';
		return 125;
	}

	std::filesystem::path Executable = std::filesystem::canonical(
		Parsed.TestCommand.front(), PathError);
	if (PathError || !std::filesystem::is_regular_file(Executable))
	{
		std::cerr << "Durin native-test application controller: test executable is invalid.\n";
		return 125;
	}
	if (!IsContainedPath(Parsed.ArtifactRoot, Executable))
	{
		std::cerr << "Durin native-test application controller: test executable escaped the artifact root.\n";
		return 125;
	}
	std::filesystem::path WorkingDirectory = std::filesystem::current_path(PathError);
	if (PathError || !IsContainedPath(Parsed.ArtifactRoot, WorkingDirectory))
	{
		std::cerr << "Durin native-test application controller: working directory is invalid.\n";
		return 125;
	}

	FRequest Request{
		Nonce,
		Executable,
		WorkingDirectory,
		static_cast<int>(getpid()),
		Parsed.ArtifactRoot,
	};
	Request.Arguments.assign(
		Parsed.TestCommand.begin() + 1, Parsed.TestCommand.end());
	std::vector<std::string> Environment;
	for (char** Entry = environ; Entry != nullptr && *Entry != nullptr; ++Entry)
	{
		Environment.emplace_back(*Entry);
	}
	if (!WriteRequestAtomic(ControlDirectory / RequestFile, Request, Error)
		|| !WriteStringVectorAtomic(ControlDirectory / EnvironmentFile, Environment, Error))
	{
		std::cerr << "Durin native-test application controller: " << Error << '\n';
		return 125;
	}

	std::vector<std::string> OpenStorage{
		"/usr/bin/open", "-n", "-W", Parsed.HostBundle.string(), "--args",
		"--durin-control-root", Parsed.ControlRoot.string(),
		"--durin-control-dir", ControlDirectory.string(), "--durin-nonce", Nonce};
	std::vector<char*> OpenArguments;
	for (std::string& Argument : OpenStorage) OpenArguments.push_back(Argument.data());
	OpenArguments.push_back(nullptr);
	pid_t OpenProcess = 0;
	const int SpawnResult = posix_spawn(&OpenProcess, "/usr/bin/open", nullptr, nullptr,
		OpenArguments.data(), environ);
	if (SpawnResult != 0)
	{
		std::cerr << "Durin native-test application controller: LaunchServices transport failed: "
			<< std::strerror(SpawnResult) << '\n';
		PruneRetainedInvocations(Parsed.ControlRoot, ControlDirectory);
		return 125;
	}

	int OpenStatus = 0;
	for (;;)
	{
		const pid_t Waited = waitpid(OpenProcess, &OpenStatus, WNOHANG);
		if (Waited == OpenProcess) break;
		if (Waited < 0 && errno != EINTR) break;
		if (GTerminationSignal != 0)
		{
			TerminateExactProcesses(ControlDirectory);
			kill(OpenProcess, SIGTERM);
			while (waitpid(OpenProcess, &OpenStatus, 0) < 0 && errno == EINTR) {}
			CopyFileToDescriptor(ControlDirectory / StandardOutputFile, STDOUT_FILENO);
			CopyFileToDescriptor(ControlDirectory / StandardErrorFile, STDERR_FILENO);
			std::cerr << "Durin native-test application controller: invocation cancelled; evidence: "
				<< ControlDirectory << '\n';
			PruneRetainedInvocations(Parsed.ControlRoot, ControlDirectory);
			return 128 + GTerminationSignal;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	CopyFileToDescriptor(ControlDirectory / StandardOutputFile, STDOUT_FILENO);
	CopyFileToDescriptor(ControlDirectory / StandardErrorFile, STDERR_FILENO);
	FResult Result;
	if (!ReadResult(ControlDirectory / ResultFile, Result, Error)
		|| !ValidateResult(Result, Nonce, Error))
	{
		std::cerr << "Durin native-test application controller: " << Error
			<< "; evidence: " << ControlDirectory << '\n';
		TerminateExactProcesses(ControlDirectory);
		PruneRetainedInvocations(Parsed.ControlRoot, ControlDirectory);
		return 125;
	}
	if (Result.Status == EResultStatus::Passed)
	{
		std::filesystem::remove_all(ControlDirectory, PathError);
		return 0;
	}
	std::cerr << "Durin native-test application controller: "
		<< ResultStatusName(Result.Status) << " during " << ResultStageName(Result.Stage);
	if (!Result.Message.empty()) std::cerr << ": " << Result.Message;
	std::cerr << "; evidence: " << ControlDirectory << '\n';
	PruneRetainedInvocations(Parsed.ControlRoot, ControlDirectory);
	if (Result.ExitCode > 0 && Result.ExitCode <= 255) return Result.ExitCode;
	return 125;
}
