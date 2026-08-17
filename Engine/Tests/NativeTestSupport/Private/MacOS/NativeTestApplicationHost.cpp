#include "NativeTestApplicationProtocol.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

namespace
{
	using namespace Durin::Testing::ApplicationHost;

	volatile sig_atomic_t GTerminationSignal = 0;

	void HandleTermination(int Signal)
	{
		GTerminationSignal = Signal;
	}

	// Describes one authenticated host invocation received from its controller.
	struct FInvocation
	{
		std::filesystem::path ControlRoot;
		std::filesystem::path ControlDirectory;
		std::string Nonce;
	};

	auto ParseInvocation(int ArgumentCount, char** Arguments,
		FInvocation& Invocation, std::string& Error) -> bool
	{
		for (int Index = 1; Index < ArgumentCount; ++Index)
		{
			const std::string_view Argument = Arguments[Index];
			if (Index + 1 >= ArgumentCount)
			{
				Error = "host option is missing a value";
				return false;
			}
			if (Argument == "--durin-control-root")
			{
				Invocation.ControlRoot = Arguments[++Index];
			}
			else if (Argument == "--durin-control-dir")
			{
				Invocation.ControlDirectory = Arguments[++Index];
			}
			else if (Argument == "--durin-nonce")
			{
				Invocation.Nonce = Arguments[++Index];
			}
			else
			{
				Error = "unknown host option: " + std::string(Argument);
				return false;
			}
		}
		if (Invocation.ControlRoot.empty() || Invocation.ControlDirectory.empty()
			|| Invocation.Nonce.empty())
		{
			Error = "host invocation is incomplete";
			return false;
		}
		return true;
	}

	auto PublishFailure(const FInvocation& Invocation, std::string Stage,
		std::string Message) -> int
	{
		FResult Result{Invocation.Nonce, std::move(Stage), "launcher-failure", 125, 0,
			std::move(Message)};
		std::string PublishError;
		if (!WriteResultAtomic(Invocation.ControlDirectory / ResultFile, Result, PublishError))
		{
			std::cerr << "Durin native-test application host could not publish failure: "
				<< PublishError << '\n';
		}
		return 125;
	}

	auto WaitForChild(pid_t Child, pid_t Controller, bool& Cancelled) -> int
	{
		int Status = 0;
		for (;;)
		{
			const pid_t Waited = waitpid(Child, &Status, WNOHANG);
			if (Waited == Child) return Status;
			if (Waited < 0 && errno != EINTR) return -1;
			if (GTerminationSignal != 0 || kill(Controller, 0) != 0)
			{
				Cancelled = true;
				kill(Child, SIGTERM);
				const auto Deadline = std::chrono::steady_clock::now()
					+ std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < Deadline)
				{
					const pid_t GraceWait = waitpid(Child, &Status, WNOHANG);
					if (GraceWait == Child) return Status;
					std::this_thread::sleep_for(std::chrono::milliseconds(25));
				}
				kill(Child, SIGKILL);
				while (waitpid(Child, &Status, 0) < 0 && errno == EINTR) {}
				return Status;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
	}
}

int main(int ArgumentCount, char** Arguments)
{
	using namespace Durin::Testing::ApplicationHost;

	FInvocation Invocation;
	std::string Error;
	if (!ParseInvocation(ArgumentCount, Arguments, Invocation, Error))
	{
		std::cerr << "Durin native-test application host admission failed: " << Error << '\n';
		return 125;
	}

	std::error_code PathError;
	Invocation.ControlRoot = std::filesystem::canonical(Invocation.ControlRoot, PathError);
	if (PathError)
	{
		std::cerr << "Durin native-test application host control root is invalid.\n";
		return 125;
	}
	Invocation.ControlDirectory = std::filesystem::canonical(
		Invocation.ControlDirectory, PathError);
	if (PathError || !IsContainedPath(Invocation.ControlRoot, Invocation.ControlDirectory))
	{
		std::cerr << "Durin native-test application host rejected the control directory.\n";
		return 125;
	}

	if (!WritePidAtomic(Invocation.ControlDirectory / HostPidFile,
		static_cast<int>(getpid()), Error))
	{
		return PublishFailure(Invocation, "host-admission", Error);
	}

	std::vector<std::string> Request;
	if (!ReadStringVector(Invocation.ControlDirectory / RequestFile, Request, Error)
		|| Request.size() < 5)
	{
		return PublishFailure(Invocation, "request-read",
			Error.empty() ? "request record is incomplete" : Error);
	}
	for (const std::string& Field : Request)
	{
		if (Field.find('\0') != std::string::npos)
		{
			return PublishFailure(Invocation, "request-validation",
				"request record contains an embedded NUL");
		}
	}
	if (Request[0] != Invocation.Nonce)
	{
		return PublishFailure(Invocation, "request-validation", "request nonce mismatch");
	}
	int ControllerPid = 0;
	const char* ControllerBegin = Request[3].data();
	const char* ControllerEnd = ControllerBegin + Request[3].size();
	const auto [ControllerPosition, ControllerError] = std::from_chars(
		ControllerBegin, ControllerEnd, ControllerPid);
	if (ControllerError != std::errc{} || ControllerPosition != ControllerEnd
		|| ControllerPid <= 1)
	{
		return PublishFailure(Invocation, "request-validation", "controller PID is invalid");
	}

	std::filesystem::path Executable = std::filesystem::canonical(Request[1], PathError);
	if (PathError || !std::filesystem::is_regular_file(Executable))
	{
		return PublishFailure(Invocation, "request-validation", "test executable is invalid");
	}
	std::filesystem::path WorkingDirectory = std::filesystem::canonical(Request[2], PathError);
	if (PathError || !std::filesystem::is_directory(WorkingDirectory))
	{
		return PublishFailure(Invocation, "request-validation", "working directory is invalid");
	}
	std::filesystem::path ArtifactRoot = std::filesystem::canonical(Request[4], PathError);
	if (PathError || !std::filesystem::is_directory(ArtifactRoot)
		|| !IsContainedPath(ArtifactRoot, Executable)
		|| !IsContainedPath(ArtifactRoot, WorkingDirectory))
	{
		return PublishFailure(Invocation, "request-validation",
			"executable or working directory escaped the artifact root");
	}

	std::vector<std::string> Environment;
	if (!ReadStringVector(Invocation.ControlDirectory / EnvironmentFile, Environment, Error))
	{
		return PublishFailure(Invocation, "environment-read", Error);
	}
	for (const std::string& Entry : Environment)
	{
		const size_t Separator = Entry.find('=');
		if (Separator == 0 || Separator == std::string::npos
			|| Entry.find('\0') != std::string::npos)
		{
			return PublishFailure(Invocation, "environment-validation",
				"environment record contains an invalid entry");
		}
	}

	const int StandardOutput = open(
		(Invocation.ControlDirectory / StandardOutputFile).c_str(),
		O_WRONLY | O_CREAT | O_EXCL, 0600);
	const int StandardError = open(
		(Invocation.ControlDirectory / StandardErrorFile).c_str(),
		O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (StandardOutput < 0 || StandardError < 0)
	{
		if (StandardOutput >= 0) close(StandardOutput);
		if (StandardError >= 0) close(StandardError);
		return PublishFailure(Invocation, "output-open", std::strerror(errno));
	}

	std::vector<std::string> ArgumentStorage;
	ArgumentStorage.reserve(Request.size() - 4);
	ArgumentStorage.push_back(Executable.string());
	ArgumentStorage.insert(ArgumentStorage.end(), Request.begin() + 5, Request.end());
	std::vector<char*> ChildArguments;
	for (std::string& Argument : ArgumentStorage) ChildArguments.push_back(Argument.data());
	ChildArguments.push_back(nullptr);
	std::vector<char*> ChildEnvironment;
	for (std::string& Entry : Environment) ChildEnvironment.push_back(Entry.data());
	ChildEnvironment.push_back(nullptr);

	posix_spawn_file_actions_t FileActions;
	posix_spawn_file_actions_init(&FileActions);
	posix_spawn_file_actions_adddup2(&FileActions, StandardOutput, STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&FileActions, StandardError, STDERR_FILENO);
	posix_spawn_file_actions_addclose(&FileActions, StandardOutput);
	posix_spawn_file_actions_addclose(&FileActions, StandardError);
	if (chdir(WorkingDirectory.c_str()) != 0)
	{
		posix_spawn_file_actions_destroy(&FileActions);
		close(StandardOutput);
		close(StandardError);
		return PublishFailure(Invocation, "child-start", std::strerror(errno));
	}

	std::signal(SIGTERM, HandleTermination);
	std::signal(SIGINT, HandleTermination);
	std::signal(SIGHUP, HandleTermination);
	pid_t Child = 0;
	const int SpawnResult = posix_spawn(&Child, Executable.c_str(), &FileActions, nullptr,
		ChildArguments.data(), ChildEnvironment.data());
	posix_spawn_file_actions_destroy(&FileActions);
	close(StandardOutput);
	close(StandardError);
	if (SpawnResult != 0)
	{
		return PublishFailure(Invocation, "child-start", std::strerror(SpawnResult));
	}
	if (!WritePidAtomic(Invocation.ControlDirectory / ChildPidFile,
		static_cast<int>(Child), Error))
	{
		kill(Child, SIGTERM);
		waitpid(Child, nullptr, 0);
		return PublishFailure(Invocation, "child-publication", Error);
	}

	bool Cancelled = false;
	const int ChildStatus = WaitForChild(Child, ControllerPid, Cancelled);
	FResult Result;
	Result.Nonce = Invocation.Nonce;
	Result.Stage = Cancelled ? "cancellation" : "test";
	if (ChildStatus < 0)
	{
		Result.Status = "launcher-failure";
		Result.ExitCode = 125;
		Result.Message = "waitpid failed: " + std::string(std::strerror(errno));
	}
	else if (Cancelled)
	{
		Result.Status = "cancelled";
		Result.ExitCode = 124;
		Result.Signal = GTerminationSignal;
	}
	else if (WIFEXITED(ChildStatus))
	{
		Result.Status = WEXITSTATUS(ChildStatus) == 0 ? "passed" : "failed";
		Result.ExitCode = WEXITSTATUS(ChildStatus);
	}
	else if (WIFSIGNALED(ChildStatus))
	{
		Result.Status = "crashed";
		Result.Signal = WTERMSIG(ChildStatus);
		Result.ExitCode = 128 + Result.Signal;
	}
	else
	{
		Result.Status = "launcher-failure";
		Result.ExitCode = 125;
		Result.Message = "child ended with an unsupported wait status";
	}
	if (!WriteResultAtomic(Invocation.ControlDirectory / ResultFile, Result, Error))
	{
		std::cerr << "Durin native-test application host completion publication failed: "
			<< Error << '\n';
		return 125;
	}
	return 0;
}
