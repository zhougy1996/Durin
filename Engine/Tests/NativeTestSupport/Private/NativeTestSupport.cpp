#include "NativeTestSupport.h"
#include "NativeTestSupportInternal.h"

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
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

		const std::filesystem::path& WorkDirectory = GetTestWorkDirectory();
		const std::filesystem::path Result =
			(WorkDirectory / RelativePath).lexically_normal();
		const std::filesystem::path RelativeResult =
			Result.lexically_relative(WorkDirectory);
		if (RelativeResult.empty()
			|| *RelativeResult.begin() == "..")
		{
			throw std::invalid_argument(
				"Native test work subdirectory must remain inside the process sandbox.");
		}

		std::filesystem::create_directories(Result);
		return std::filesystem::weakly_canonical(Result);
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
				State.WorkDirectory = Private::CreateUniqueRunDirectory(
					WorkRoot,
					FPlatformProcess::CurrentProcessId(),
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
