#include "NativeTestSupport.h"
#include "NativeTestSupportInternal.h"

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Durin::Testing
{
	namespace
	{
		struct FNativeTestProcessState
		{
			std::filesystem::path WorkDirectory;
			bool KeepWork = false;
			bool Initialized = false;
		};

		auto GetProcessState() -> FNativeTestProcessState&
		{
			static FNativeTestProcessState State;
			return State;
		}

		auto GetProcessStateMutex() -> std::mutex&
		{
			static std::mutex Mutex;
			return Mutex;
		}

		auto MakeNonce() -> std::string
		{
			std::string Nonce = FGuid::NewGuid().ToString();
			std::erase(Nonce, '-');
			return Nonce;
		}

		auto ResolveContainedWorkPath(const std::filesystem::path& Path)
			-> std::filesystem::path
		{
			if (Path.empty())
			{
				throw std::invalid_argument(
					"Native test work path must not be empty.");
			}

			const std::filesystem::path WorkDirectory =
				std::filesystem::weakly_canonical(GetTestWorkDirectory());
			const std::filesystem::path Candidate =
				std::filesystem::weakly_canonical(
					Path.is_absolute() ? Path : WorkDirectory / Path);
			const std::filesystem::path Relative =
				Candidate.lexically_relative(WorkDirectory);
			if (Relative.empty()
				|| Relative == "."
				|| *Relative.begin() == "..")
			{
				throw std::invalid_argument(
					"Native test work path must remain below the process sandbox.");
			}
			return Candidate;
		}

		constexpr std::string_view SuccessfulRunMarker = ".durin-success";

		auto IsProcessRunning(const std::uint32_t ProcessId) -> bool
		{
#if PLATFORM_WINDOWS
			const HANDLE Process = OpenProcess(SYNCHRONIZE, FALSE, ProcessId);
			if (Process == nullptr)
			{
				return GetLastError() != ERROR_INVALID_PARAMETER;
			}
			const DWORD WaitResult = WaitForSingleObject(Process, 0);
			CloseHandle(Process);
			return WaitResult == WAIT_TIMEOUT || WaitResult == WAIT_FAILED;
#else
			return true;
#endif
		}

		auto MarkRunSuccessful(const std::filesystem::path& WorkDirectory) -> void
		{
			std::ofstream Marker(
				WorkDirectory / SuccessfulRunMarker,
				std::ios::binary | std::ios::trunc);
			Marker << "completed\n";
		}

		auto ConsumeFlag(int& ArgumentCount, char** Arguments, const std::string_view Flag) -> bool
		{
			bool Found = false;
			int OutputIndex = 1;
			for (int Index = 1; Index < ArgumentCount; ++Index)
			{
				if (Arguments[Index] != nullptr && Flag == Arguments[Index])
				{
					Found = true;
					continue;
				}
				Arguments[OutputIndex++] = Arguments[Index];
			}
			ArgumentCount = OutputIndex;
			return Found;
		}

		class FNativeTestSandboxListener final : public ::testing::EmptyTestEventListener
		{
		public:
			explicit FNativeTestSandboxListener(
				std::filesystem::path WorkDirectory,
				bool KeepWork,
				bool& ProgramEnded)
				: WorkDirectory(std::move(WorkDirectory))
				, KeepWork(KeepWork)
				, ProgramEnded(&ProgramEnded)
			{
			}

			auto OnTestProgramEnd(const ::testing::UnitTest& UnitTest) -> void override
			{
				*ProgramEnded = true;
				if (!UnitTest.Passed() || KeepWork)
				{
					std::cout << "[ DURIN   ] Preserved test work directory: "
						<< WorkDirectory.string() << '\n';
					return;
				}

				MarkRunSuccessful(WorkDirectory);
				const char* ForceCleanupFailure =
					std::getenv("DURIN_TEST_FORCE_CLEANUP_FAILURE");
				if (ForceCleanupFailure != nullptr
					&& std::string_view(ForceCleanupFailure) == "1")
				{
					std::cerr << "[ DURIN   ] Failed to clean test work directory "
						<< WorkDirectory.string() << ": forced cleanup failure\n";
					return;
				}

				std::error_code ErrorCode;
				std::filesystem::remove_all(WorkDirectory, ErrorCode);
				if (ErrorCode)
				{
					std::cerr << "[ DURIN   ] Failed to clean test work directory "
						<< WorkDirectory.string() << ": " << ErrorCode.message() << '\n';
				}
			}

		private:
			std::filesystem::path WorkDirectory;
			bool KeepWork;
			bool* ProgramEnded;
		};
	}

	auto Private::CreateUniqueRunDirectory(
		const std::filesystem::path& WorkRoot,
		const std::uint32_t ProcessId,
		const Private::FNonceGenerator& NonceGenerator) -> std::filesystem::path
	{
		const std::filesystem::path RunsRoot =
			std::filesystem::absolute(WorkRoot / "Runs").lexically_normal();
		std::filesystem::create_directories(RunsRoot);

		for (std::uint32_t Attempt = 0; Attempt != 64; ++Attempt)
		{
			const std::filesystem::path Candidate =
				RunsRoot / ("run-p" + std::to_string(ProcessId) + "-" + NonceGenerator());
			std::error_code ErrorCode;
			if (std::filesystem::create_directory(Candidate, ErrorCode))
			{
				return std::filesystem::weakly_canonical(Candidate);
			}
			if (ErrorCode)
			{
				throw std::filesystem::filesystem_error(
					"Failed to create native-test process sandbox",
					Candidate,
					ErrorCode);
			}
		}

		throw std::runtime_error(
			"Failed to allocate a unique native-test process sandbox after 64 attempts.");
	}

	auto Private::CleanupAbandonedSuccessfulRunDirectories(
		const std::filesystem::path& WorkRoot,
		const std::uint32_t CurrentProcessId,
		const std::filesystem::file_time_type CurrentTime,
		const std::filesystem::file_time_type::duration MinimumAge,
		const FProcessRunningPredicate& IsProcessRunning) -> std::uintmax_t
	{
		const std::filesystem::path RunsRoot =
			std::filesystem::absolute(WorkRoot / "Runs").lexically_normal();
		std::error_code IteratorError;
		std::filesystem::directory_iterator Iterator(RunsRoot, IteratorError);
		if (IteratorError)
		{
			return 0;
		}

		const std::regex RunPattern("run-p([0-9]+)-[0-9a-f]+");
		std::uintmax_t RemovedCount = 0;
		for (const std::filesystem::directory_entry& Entry : Iterator)
		{
			if (!Entry.is_directory())
			{
				continue;
			}
			std::smatch Match;
			const std::string Name = Entry.path().filename().string();
			if (!std::regex_match(Name, Match, RunPattern))
			{
				continue;
			}

			const std::filesystem::path Marker =
				Entry.path() / SuccessfulRunMarker;
			std::error_code TimeError;
			const std::filesystem::file_time_type CompletionTime =
				std::filesystem::last_write_time(Marker, TimeError);
			if (TimeError || CurrentTime - CompletionTime < MinimumAge)
			{
				continue;
			}

			const std::uint64_t ParsedProcessId = std::stoull(Match[1].str());
			if (ParsedProcessId > std::numeric_limits<std::uint32_t>::max())
			{
				continue;
			}
			const auto ProcessId = static_cast<std::uint32_t>(ParsedProcessId);
			if (ProcessId == CurrentProcessId || IsProcessRunning(ProcessId))
			{
				continue;
			}

			std::error_code CleanupError;
			RemovedCount += std::filesystem::remove_all(
				Entry.path(),
				CleanupError);
		}
		return RemovedCount;
	}

	auto GetTestWorkDirectory() -> const std::filesystem::path&
	{
		const FNativeTestProcessState& State = GetProcessState();
		if (!State.Initialized)
		{
			throw std::logic_error(
				"Native test work directory was requested before the Durin test harness initialized.");
		}
		return State.WorkDirectory;
	}

	auto CreateTestWorkSubdirectory(const std::filesystem::path& RelativePath)
		-> std::filesystem::path
	{
		if (RelativePath.empty() || RelativePath.is_absolute())
		{
			throw std::invalid_argument(
				"Native test work subdirectory must be a non-empty relative path.");
		}

		const std::filesystem::path Result = ResolveContainedWorkPath(RelativePath);
		std::filesystem::create_directories(Result);
		return std::filesystem::weakly_canonical(Result);
	}

	auto CreateTestFixtureDirectory(const std::filesystem::path& RelativePath)
		-> std::filesystem::path
	{
		if (RelativePath.empty() || RelativePath.is_absolute())
		{
			throw std::invalid_argument(
				"Native test fixture directory must be a non-empty relative path.");
		}
		const std::filesystem::path Result = ResolveContainedWorkPath(RelativePath);
		std::filesystem::remove_all(Result);
		std::filesystem::create_directories(Result);
		return std::filesystem::weakly_canonical(Result);
	}

	auto RemoveTestWorkDirectory(const std::filesystem::path& Path)
		-> std::uintmax_t
	{
		return std::filesystem::remove_all(ResolveContainedWorkPath(Path));
	}

	auto RemoveTestWorkDirectory(
		const std::filesystem::path& Path,
		std::error_code& ErrorCode) noexcept -> std::uintmax_t
	{
		try
		{
			return std::filesystem::remove_all(
				ResolveContainedWorkPath(Path),
				ErrorCode);
		}
		catch (const std::invalid_argument&)
		{
			ErrorCode = std::make_error_code(std::errc::permission_denied);
			return 0;
		}
		catch (const std::filesystem::filesystem_error& Exception)
		{
			ErrorCode = Exception.code();
			return 0;
		}
	}

	auto IsTestWorkDirectoryKept() -> bool
	{
		return GetProcessState().KeepWork;
	}

	auto RunNativeTests(
		int ArgumentCount,
		char** Arguments,
		const std::filesystem::path& WorkRoot) -> int
	{
		const bool KeepWorkArgument =
			ConsumeFlag(ArgumentCount, Arguments, "--durin-keep-test-work");
		const bool CrashProbe =
			ConsumeFlag(ArgumentCount, Arguments, "--durin-crash-after-sandbox-create");
		const char* KeepWorkEnvironment = std::getenv("DURIN_TEST_KEEP_WORK");
		const bool KeepWork =
			KeepWorkArgument
			|| (KeepWorkEnvironment != nullptr && std::string_view(KeepWorkEnvironment) == "1");

		std::filesystem::path WorkDirectory;
		{
			std::scoped_lock Lock(GetProcessStateMutex());
			FNativeTestProcessState& State = GetProcessState();
			if (!State.Initialized)
			{
				const std::uint32_t ProcessId =
					FPlatformProcess::CurrentProcessId();
				Private::CleanupAbandonedSuccessfulRunDirectories(
					WorkRoot,
					ProcessId,
					std::filesystem::file_time_type::clock::now(),
					std::chrono::hours(24),
					IsProcessRunning);
				State.WorkDirectory = Private::CreateUniqueRunDirectory(
					WorkRoot,
					ProcessId,
					MakeNonce);
				State.KeepWork = KeepWork;
				State.Initialized = true;
			}
			WorkDirectory = State.WorkDirectory;
		}

		std::cout << "[ DURIN   ] Test work directory: "
			<< WorkDirectory.string() << '\n';
		std::cout.flush();
		if (CrashProbe)
		{
			// Model a crash by bypassing stack unwinding and listeners without
			// invoking the interactive Debug CRT abort dialog on Windows.
			std::_Exit(3);
		}

		::testing::InitGoogleTest(&ArgumentCount, Arguments);
		bool ProgramListenerRan = false;
		::testing::UnitTest::GetInstance()->listeners().Append(
			new FNativeTestSandboxListener(
				WorkDirectory,
				KeepWork,
				ProgramListenerRan));
		const int Result = RUN_ALL_TESTS();

		// GoogleTest does not dispatch test-program listener events for
		// --gtest_list_tests, which is used during CTest discovery.
		if (!ProgramListenerRan
			&& Result == 0
			&& !KeepWork
			&& std::filesystem::exists(WorkDirectory))
		{
			MarkRunSuccessful(WorkDirectory);
			const char* ForceCleanupFailure =
				std::getenv("DURIN_TEST_FORCE_CLEANUP_FAILURE");
			if (ForceCleanupFailure == nullptr
				|| std::string_view(ForceCleanupFailure) != "1")
			{
				std::error_code ErrorCode;
				std::filesystem::remove_all(WorkDirectory, ErrorCode);
				if (ErrorCode)
				{
					std::cerr << "[ DURIN   ] Failed to clean test work directory "
						<< WorkDirectory.string() << ": " << ErrorCode.message() << '\n';
				}
			}
		}
		return Result;
	}
}
