#pragma once

#include "AssetCompatibilityAudit.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Threading/ThreadEvent.h"

#include "NativeTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>

namespace
{
	auto MakePath(std::string_view Value) -> Durin::FPackagePath
	{
		Durin::FPackagePath Path;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Path));
		return Path;
	}

	auto MakeData(std::string_view Path, uintmax_t Size = 10, int64 Ticks = 20)
		-> Durin::FAssetData
	{
		Durin::FPackagePath AssetPath = MakePath(Path);
		return {
			.PackagePath = AssetPath,
			.PhysicalPath = std::format("C:/fixtures/{}.dasset", AssetPath.GetPackageName()),
			.FileSize = Size,
			.LastWriteTimeTicks = Ticks,
		};
	}

	auto MakeCompletedRecord(const Durin::FAssetPackageCompatibilityProbeInput& Input)
		-> Durin::FAssetPackageCompatibilityProbeResult
	{
		return {
			.Status = Durin::EAssetCompatibilityProbeStatus::Completed,
			.Record = Durin::FAssetPackageCompatibilityRecord{
				.PackagePath = Input.PackagePath,
				.PhysicalPath = Input.PhysicalPath,
				.Fingerprint = {
					.FileSize = Input.ExpectedFileSize,
					.LastWriteTimeTicks = Input.ExpectedLastWriteTimeTicks,
				},
				.Inspection = Durin::EAssetCompatibilityInspection::Ready,
				.Compatibility = Durin::EAssetPackageCompatibility::Compatible,
			},
		};
	}

	template<typename TPredicate>
	auto WaitUntil(TPredicate&& Predicate) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::PumpGameThreadDeferredWork();
			if (Predicate()) return true;
			std::this_thread::yield();
		}
		return Predicate();
	}

	class FAssetCompatibilityAuditFixture : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!Durin::GIsGameThreadIdInitialized)
			{
				Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
				Durin::GIsGameThreadIdInitialized = true;
			}
			Root = Durin::Testing::GetTestWorkDirectory() / "AssetCompatibilityAudit";
			std::filesystem::create_directories(Root);
			const std::array Definitions{
				Durin::FMountPoint{
					.VirtualRoot = "/AuditTests/",
					.Owner = Durin::EMountOwner::ActiveProject,
					.Root = Root,
					.bAutoScan = true}};
			Mounts = std::make_unique<Durin::Testing::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Mounts->IsValid()) << Mounts->GetError();
			const Durin::FTaskSchedulerDiagnostics Diagnostics =
				Durin::GetTaskSchedulerDiagnostics();
			bRestoreScheduler = Diagnostics.bRunning;
			PreviousConfig.NumWorkerThreads = Diagnostics.WorkerCount;
			PreviousConfig.MaxNonterminalTasks = Diagnostics.TaskReservationCapacity;
			bRestoreDeferredExecutor =
				Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting;
			Durin::ShutdownTaskScheduler(false);
			ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
			ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
		}

		void TearDown() override
		{
			Durin::ShutdownTaskScheduler(true);
			if (bRestoreScheduler && !Durin::InitializeTaskScheduler(PreviousConfig))
			{
				ADD_FAILURE() << "Failed to restore the native-test task scheduler.";
			}
			if (bRestoreDeferredExecutor && !Durin::InitializeGameThreadDeferredExecutor())
			{
				ADD_FAILURE() << "Failed to restore the native-test deferred executor.";
			}
			Mounts.reset();
		}

		std::filesystem::path Root;
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> Mounts;
		Durin::FTaskSchedulerConfig PreviousConfig;
		bool bRestoreScheduler = false;
		bool bRestoreDeferredExecutor = false;
	};
}
