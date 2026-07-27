#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
		const auto ControlRoot = std::filesystem::path(DURIN_TEST_WORK_DIR) / "ProcessIsolationProbeControl";
		const char* ProbeMode = std::getenv("DURIN_TEST_ISOLATION_PROBE_MODE");
		if (ProbeMode == nullptr)
		{
			GTEST_SKIP() << "Set DURIN_TEST_ISOLATION_PROBE_MODE to legacy or isolated.";
		}
		const bool UseIsolatedRoot = ProbeMode != nullptr && std::string(ProbeMode) == "isolated";
		const auto SharedRoot = std::filesystem::path(DURIN_TEST_WORK_DIR) / "ProcessIsolationProbe";
		const auto WritableRoot = UseIsolatedRoot ? SharedRoot / Owner : SharedRoot;
		std::filesystem::create_directories(ControlRoot);
		std::filesystem::create_directories(WritableRoot);

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
			<< "Both CTest processes resolved the same logical filename below DURIN_TEST_WORK_DIR.";
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
