#include <gtest/gtest.h>

#include "NativeTestSupport.h"

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
		const char* ControlRootValue = std::getenv("DURIN_TEST_ISOLATION_PROBE_CONTROL");
		ASSERT_NE(ControlRootValue, nullptr)
			<< "Run the NativeTestIsolationProbeCharacterization target.";
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
			<< "Run both discovered probe cases concurrently with CTest -j 2.";

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
