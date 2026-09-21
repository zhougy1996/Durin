#include "AssetCompatibilityAuditTestSupport.h"

#include <iostream>

using FAssetCompatibilityAuditQualificationTests = FAssetCompatibilityAuditFixture;

TEST_F(FAssetCompatibilityAuditQualificationTests, CancellationPublishesNoPartialRecordForTheInterruptedPackage)
{
	std::atomic_bool Started = false;
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[&Started](const auto&, const auto&, const auto& IsCancelled) {
			Started = true;
			while (!IsCancelled()) std::this_thread::yield();
			return Durin::FAssetPackageCompatibilityProbeResult{
				.Status = Durin::EAssetCompatibilityProbeStatus::Cancelled};
		});
	const auto Data = MakeData("/AuditTests/Cancel");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{{Data.PackagePath, Data}};

	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	ASSERT_TRUE(WaitUntil([&] { return Started.load(); }));
	const auto CancelStarted = std::chrono::steady_clock::now();
	Model.CancelAndDrain();
	const auto CancellationLatency = std::chrono::steady_clock::now() - CancelStarted;

	EXPECT_EQ(Model.GetState(), Durin::Editor::EAssetCompatibilityAuditState::Cancelled);
	EXPECT_EQ(Model.GetProgress().Completed, 0u);
	const auto* Record = Model.FindRecord(Data.PackagePath);
	ASSERT_NE(Record, nullptr);
	EXPECT_EQ(Record->Inspection, Durin::EAssetCompatibilityInspection::NotChecked);
	EXPECT_LT(CancellationLatency, std::chrono::seconds(1));
	std::cout << "[ QUALIFICATION ] asset_compatibility cancellation_us="
		<< std::chrono::duration_cast<std::chrono::microseconds>(CancellationLatency).count()
		<< '\n';
}

TEST_F(FAssetCompatibilityAuditQualificationTests, RepresentativeCorpusMeasuresWorkerAndMailboxCosts)
{
	constexpr uint32 PackageCount = 32;
	std::atomic_uint32_t ProbeCount = 0;
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[&ProbeCount](const auto& Input, const auto&, const auto&) {
			++ProbeCount;
			return MakeCompletedRecord(Input);
		});
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets;
	for (uint32 Index = 0; Index < PackageCount; ++Index)
	{
		const auto Data = MakeData(std::format("/AuditTests/Qualification{:02}", Index));
		Assets.emplace(Data.PackagePath, Data);
	}

	const auto WorkerStarted = std::chrono::steady_clock::now();
	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	std::chrono::steady_clock::duration PeakMailboxDuration{};
	ASSERT_TRUE(WaitUntil([&] {
		const auto TickStarted = std::chrono::steady_clock::now();
		Model.Tick(Assets);
		PeakMailboxDuration = std::max(
			PeakMailboxDuration, std::chrono::steady_clock::now() - TickStarted);
		return Model.GetState() == Durin::Editor::EAssetCompatibilityAuditState::Completed;
	}));
	const auto WorkerDuration = std::chrono::steady_clock::now() - WorkerStarted;

	EXPECT_EQ(Model.GetState(), Durin::Editor::EAssetCompatibilityAuditState::Completed);
	EXPECT_EQ(Model.GetProgress().Completed, PackageCount);
	EXPECT_EQ(ProbeCount.load(), PackageCount);
	EXPECT_LT(WorkerDuration, std::chrono::seconds(3));
	EXPECT_LT(PeakMailboxDuration, std::chrono::milliseconds(100));
	std::cout << "[ QUALIFICATION ] asset_compatibility packages=" << PackageCount
		<< " worker_us="
		<< std::chrono::duration_cast<std::chrono::microseconds>(WorkerDuration).count()
		<< " peak_mailbox_tick_us="
		<< std::chrono::duration_cast<std::chrono::microseconds>(PeakMailboxDuration).count()
		<< '\n';
}
