#include <gtest/gtest.h>

#include "NativeTestSupport.h"
#include "NativeTestSupportInternal.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <utility>

namespace
{
	using namespace std::chrono_literals;

	class FReadyFileGuard
	{
	public:
		explicit FReadyFileGuard(std::filesystem::path Path)
			: Path(std::move(Path))
		{
		}

		~FReadyFileGuard()
		{
			std::error_code ErrorCode;
			std::filesystem::remove(Path, ErrorCode);
		}

	private:
		std::filesystem::path Path;
	};

	void RunSharedRootCollisionProbe(const std::string& Owner, const std::string& Peer)
	{
		const char* ControlRootValue = std::getenv("DURIN_TEST_ISOLATION_PROBE_CONTROL");
		if (ControlRootValue == nullptr)
		{
			GTEST_SKIP() << "Run the NativeTestIsolationProbeCharacterization target.";
		}
		const auto ControlRoot = std::filesystem::path(ControlRootValue);
		const auto WritableRoot =
			Durin::Testing::CreateTestWorkSubdirectory("ProcessIsolationProbe");
		std::filesystem::create_directories(ControlRoot);

		const auto ReadyPath = ControlRoot / ("ready-" + Owner);
		const auto PeerReadyPath = ControlRoot / ("ready-" + Peer);
		const auto SharedPath = WritableRoot / "same-logical-name.txt";
		FReadyFileGuard ReadyGuard(ReadyPath);

		{
			std::ofstream ReadyStream(ReadyPath, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(ReadyStream);
			ReadyStream << Owner;
		}

		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (!std::filesystem::exists(PeerReadyPath) && std::chrono::steady_clock::now() < Deadline)
		{
			std::this_thread::sleep_for(10ms);
		}
		ASSERT_TRUE(std::filesystem::exists(PeerReadyPath))
			<< "Run both discovered probe cases concurrently with CTest --run-disabled -j 2.";

		{
			std::ofstream SharedStream(SharedPath, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(SharedStream);
			SharedStream << Owner;
		}

		std::this_thread::sleep_for(250ms);

		std::string ObservedOwner;
		{
			std::ifstream SharedStream(SharedPath, std::ios::binary);
			ASSERT_TRUE(SharedStream);
			SharedStream >> ObservedOwner;
		}
		EXPECT_EQ(ObservedOwner, Owner)
			<< "Both CTest processes resolved the same native-test process sandbox.";
	}
}

TEST(FNativeTestProcessIsolationProbeTests, ProcessAOwnsSameLogicalFilename)
{
	RunSharedRootCollisionProbe("process-a", "process-b");
}

TEST(FNativeTestProcessIsolationProbeTests, ProcessBOwnsSameLogicalFilename)
{
	RunSharedRootCollisionProbe("process-b", "process-a");
}

TEST(FNativeTestProcessSandboxTests, ProvidesCanonicalUniqueRunDirectory)
{
	const std::filesystem::path& WorkDirectory =
		Durin::Testing::GetTestWorkDirectory();
	EXPECT_EQ(WorkDirectory, std::filesystem::weakly_canonical(WorkDirectory));
	EXPECT_TRUE(std::filesystem::is_directory(WorkDirectory));
	EXPECT_TRUE(std::regex_match(
		WorkDirectory.filename().string(),
		std::regex("run-p[0-9]+-[0-9a-f]{32}")));
	EXPECT_EQ(&WorkDirectory, &Durin::Testing::GetTestWorkDirectory());
}

TEST(FNativeTestProcessSandboxTests, CreatesContainedUnicodeAndLongSubdirectories)
{
	const std::filesystem::path UnicodeDirectory =
		Durin::Testing::CreateTestWorkSubdirectory(
			std::filesystem::path(L"路径") / L"夹具");
	EXPECT_TRUE(std::filesystem::is_directory(UnicodeDirectory));

	std::filesystem::path LongRelativePath;
	for (int Index = 0; Index != 12; ++Index)
	{
		LongRelativePath /= "long-path-segment";
	}
	const std::filesystem::path LongDirectory =
		Durin::Testing::CreateTestWorkSubdirectory(LongRelativePath);
	EXPECT_TRUE(std::filesystem::is_directory(LongDirectory));
	EXPECT_GT(LongDirectory.native().size(), 260u);
}

TEST(FNativeTestProcessSandboxTests, RejectsPathsOutsideTheProcessSandbox)
{
	EXPECT_THROW(
		(void)Durin::Testing::CreateTestWorkSubdirectory(
			std::filesystem::path("..") / "escape"),
		std::invalid_argument);
	EXPECT_THROW(
		(void)Durin::Testing::CreateTestWorkSubdirectory(
			std::filesystem::current_path()),
		std::invalid_argument);
	EXPECT_THROW(
		(void)Durin::Testing::CreateTestFixtureDirectory(
			std::filesystem::current_path()),
		std::invalid_argument);
	EXPECT_THROW(
		(void)Durin::Testing::RemoveTestWorkDirectory(
			Durin::Testing::GetTestWorkDirectory()),
		std::invalid_argument);
	EXPECT_THROW(
		(void)Durin::Testing::RemoveTestWorkDirectory(
			Durin::Testing::GetTestWorkDirectory().parent_path()),
		std::invalid_argument);
}

TEST(FNativeTestProcessSandboxTests, RecreatesAndSafelyRemovesNamedFixtureDirectories)
{
	const std::filesystem::path Fixture =
		Durin::Testing::CreateTestFixtureDirectory("NamedFixture");
	std::ofstream(Fixture / "stale.txt") << "stale";

	EXPECT_EQ(
		Durin::Testing::CreateTestFixtureDirectory("NamedFixture"),
		Fixture);
	EXPECT_FALSE(std::filesystem::exists(Fixture / "stale.txt"));
	EXPECT_TRUE(std::filesystem::is_directory(Fixture));
	EXPECT_GT(Durin::Testing::RemoveTestWorkDirectory(Fixture), 0u);
	EXPECT_FALSE(std::filesystem::exists(Fixture));
}

TEST(FNativeTestProcessSandboxTests, RetriesWhenAReusedProcessIdCollides)
{
	const std::filesystem::path WorkRoot =
		Durin::Testing::CreateTestWorkSubdirectory("ProcessIdReuse");
	constexpr std::uint32_t ReusedProcessId = 4242;
	const std::string OccupiedNonce(32, 'a');
	const std::string AvailableNonce(32, 'b');
	const std::filesystem::path OccupiedDirectory =
		WorkRoot / "Runs" / ("run-p4242-" + OccupiedNonce);
	std::filesystem::create_directories(OccupiedDirectory);

	int NonceRequestCount = 0;
	const std::filesystem::path AllocatedDirectory =
		Durin::Testing::Private::CreateUniqueRunDirectory(
			WorkRoot,
			ReusedProcessId,
			[&]() {
				return NonceRequestCount++ == 0 ? OccupiedNonce : AvailableNonce;
			});

	EXPECT_EQ(NonceRequestCount, 2);
	EXPECT_EQ(
		AllocatedDirectory.filename(),
		std::filesystem::path("run-p4242-" + AvailableNonce));
	EXPECT_TRUE(std::filesystem::is_directory(OccupiedDirectory));
}

TEST(FNativeTestProcessSandboxTests, CleansOnlyAbandonedSuccessfulRuns)
{
	const std::filesystem::path CleanupRoot =
		Durin::Testing::CreateTestFixtureDirectory("AbandonedRunCleanup");
	const std::filesystem::path RunsRoot = CleanupRoot / "Runs";
	const std::filesystem::path Abandoned =
		RunsRoot / "run-p111-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	const std::filesystem::path Live =
		RunsRoot / "run-p222-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	const std::filesystem::path Current =
		RunsRoot / "run-p333-cccccccccccccccccccccccccccccccc";
	for (const std::filesystem::path& Run : {Abandoned, Live, Current})
	{
		std::filesystem::create_directories(Run);
		std::ofstream(Run / ".durin-success") << "completed\n";
	}

	const auto CurrentTime = std::filesystem::file_time_type::clock::now();
	for (const std::filesystem::path& Run : {Abandoned, Live, Current})
	{
		std::filesystem::last_write_time(
			Run / ".durin-success",
			CurrentTime - 48h);
	}

	EXPECT_GT(
		Durin::Testing::Private::CleanupAbandonedSuccessfulRunDirectories(
			CleanupRoot,
			333,
			CurrentTime,
			24h,
			[](const std::uint32_t ProcessId) {
				return ProcessId == 222;
			}),
		0u);
	EXPECT_FALSE(std::filesystem::exists(Abandoned));
	EXPECT_TRUE(std::filesystem::exists(Live));
	EXPECT_TRUE(std::filesystem::exists(Current));
}
