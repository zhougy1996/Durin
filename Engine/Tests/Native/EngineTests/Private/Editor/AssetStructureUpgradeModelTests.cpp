#include "Documents/AssetStructureUpgradeModel.h"

#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeReport(
		std::string_view Name,
		Durin::Asset::EAssetCompatibilityClassification Classification,
		Durin::Asset::EAssetCompatibilityRisk Risk) -> Durin::Asset::FAssetLoadReport
	{
		InitializeDObjectSystem();
		static const bool bMounted = [] {
		Durin::PathUtilities::RegisterMountPointForTests(
				"/UpgradeModel/",
				(Durin::Testing::GetTestWorkDirectory() / "UpgradeModel").generic_string() + "/");
			return true;
		}();
		(void)bMounted;
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(std::format("/UpgradeModel/{}", Name), Path));
		return {
			.PackagePath = Path,
			.CompatibilityIssues = {{
				.ObjectPath = std::format("/UpgradeModel/{}:Object", Name),
				.DeclaringClass = "Tests::DLegacyObject",
				.LegacyFields = {{.Name = "LegacyField"}},
				.Classification = Classification,
				.MigrationSummary = "Test compatibility change.",
				.Risk = Risk}}};
	}

	auto MakeLevel(std::string_view Name) -> Durin::DLevel*
	{
		static std::array<std::byte, 64> Tokens{};
		const size_t Index = std::hash<std::string_view>{}(Name) % Tokens.size();
		return reinterpret_cast<Durin::DLevel*>(&Tokens[Index]);
	}

	struct FOperationRecorder
	{
		bool bSaveSucceeds = true;
		bool bActivationSucceeds = true;
		Durin::uint32 SaveCount = 0;
		Durin::uint32 ActivationCount = 0;
		Durin::uint32 UnloadCount = 0;
		Durin::uint32 CompletionCount = 0;
		bool bLastSaveAllowedDataLoss = false;
		bool bLastCompletionSucceeded = false;
		Durin::FAssetPath UnloadedPath;

		auto MakeOperations() -> Durin::FAssetStructureUpgradeOperations
		{
			return {
				.Save = [this](Durin::DLevel*, bool bAllowDataLoss) {
					++SaveCount;
					bLastSaveAllowedDataLoss = bAllowDataLoss;
					return bSaveSucceeds;
				},
				.Activate = [this](Durin::DLevel*) {
					++ActivationCount;
					return bActivationSucceeds;
				},
				.Unload = [this](const Durin::FAssetPath& Path) {
					++UnloadCount;
					UnloadedPath = Path;
				},
				.CompleteDeferredOpen = [this](bool bSucceeded) {
					++CompletionCount;
					bLastCompletionSucceeded = bSucceeded;
				}};
		}
	};
} // namespace

TEST(FAssetStructureUpgradeModelTests, ClassificationsExposeSafeAndRiskySaveActions)
{
	Durin::FAssetStructureUpgradeModel Model;
	ASSERT_TRUE(Model.Begin(
		MakeLevel("Safe"),
		MakeReport(
			"Safe",
			Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
			Durin::Asset::EAssetCompatibilityRisk::None),
		false));
	EXPECT_TRUE(Model.CanSaveWithoutDataLoss());
	EXPECT_FALSE(Model.CanDiscardIncompatibleData());

	FOperationRecorder Recorder;
	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::Cancel, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Cancelled);

	ASSERT_TRUE(Model.Begin(
		MakeLevel("Migrated"),
		MakeReport(
			"Migrated",
			Durin::Asset::EAssetCompatibilityClassification::Migrated,
			Durin::Asset::EAssetCompatibilityRisk::None),
		false));
	EXPECT_TRUE(Model.CanSaveWithoutDataLoss());
	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::Cancel, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Cancelled);

	ASSERT_TRUE(Model.Begin(
		MakeLevel("Risky"),
		MakeReport(
			"Risky",
			Durin::Asset::EAssetCompatibilityClassification::DataLossRisk,
			Durin::Asset::EAssetCompatibilityRisk::PotentialDataLoss),
		false));
	EXPECT_FALSE(Model.CanSaveWithoutDataLoss());
	EXPECT_TRUE(Model.CanDiscardIncompatibleData());
	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::Cancel, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Cancelled);

	ASSERT_TRUE(Model.Begin(
		MakeLevel("Unknown"),
		MakeReport(
			"Unknown",
			Durin::Asset::EAssetCompatibilityClassification::UnknownIncompatible,
			Durin::Asset::EAssetCompatibilityRisk::UnknownNewerSchema),
		false));
	EXPECT_FALSE(Model.CanSaveWithoutDataLoss());
	EXPECT_TRUE(Model.CanDiscardIncompatibleData());
}

TEST(FAssetStructureUpgradeModelTests, SafeSaveActivatesAndCompletesDeferredOpen)
{
	Durin::FAssetStructureUpgradeModel Model;
	ASSERT_TRUE(Model.Begin(
		MakeLevel("Save"),
		MakeReport(
			"Save",
			Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
			Durin::Asset::EAssetCompatibilityRisk::None),
		true));
	FOperationRecorder Recorder;

	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::SaveAndOpen, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Activated);
	EXPECT_EQ(Recorder.SaveCount, 1u);
	EXPECT_FALSE(Recorder.bLastSaveAllowedDataLoss);
	EXPECT_EQ(Recorder.ActivationCount, 1u);
	EXPECT_EQ(Recorder.UnloadCount, 0u);
	EXPECT_EQ(Recorder.CompletionCount, 1u);
	EXPECT_TRUE(Recorder.bLastCompletionSucceeded);
	EXPECT_FALSE(Model.IsPending());
}

TEST(FAssetStructureUpgradeModelTests, RiskySaveRequiresExplicitDataLossDecision)
{
	Durin::FAssetStructureUpgradeModel Model;
	ASSERT_TRUE(Model.Begin(
		MakeLevel("RiskSave"),
		MakeReport(
			"RiskSave",
			Durin::Asset::EAssetCompatibilityClassification::UnknownIncompatible,
			Durin::Asset::EAssetCompatibilityRisk::UnknownNewerSchema),
		true));
	FOperationRecorder Recorder;

	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::SaveAndOpen, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Rejected);
	EXPECT_TRUE(Model.IsPending());
	EXPECT_EQ(Recorder.SaveCount, 0u);

	EXPECT_EQ(
		Model.Resolve(
			Durin::EAssetStructureUpgradeDecision::DiscardIncompatibleDataSaveAndOpen,
			Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Activated);
	EXPECT_EQ(Recorder.SaveCount, 1u);
	EXPECT_TRUE(Recorder.bLastSaveAllowedDataLoss);
	EXPECT_EQ(Recorder.CompletionCount, 1u);
	EXPECT_TRUE(Recorder.bLastCompletionSucceeded);
}

TEST(FAssetStructureUpgradeModelTests, OpenWithoutSavingActivatesAndKeepsPersistenceUntouched)
{
	Durin::FAssetStructureUpgradeModel Model;
	ASSERT_TRUE(Model.Begin(
		MakeLevel("NoSave"),
		MakeReport(
			"NoSave",
			Durin::Asset::EAssetCompatibilityClassification::Migrated,
			Durin::Asset::EAssetCompatibilityRisk::None),
		true));
	FOperationRecorder Recorder;

	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::OpenWithoutSaving, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Activated);
	EXPECT_EQ(Recorder.SaveCount, 0u);
	EXPECT_EQ(Recorder.ActivationCount, 1u);
	EXPECT_EQ(Recorder.CompletionCount, 1u);
	EXPECT_TRUE(Recorder.bLastCompletionSucceeded);
}

TEST(FAssetStructureUpgradeModelTests, CancelUnloadsPendingPackageAndRejectsDeferredOpen)
{
	Durin::FAssetStructureUpgradeModel Model;
	const Durin::Asset::FAssetLoadReport Report = MakeReport(
		"Cancel",
		Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
		Durin::Asset::EAssetCompatibilityRisk::None);
	const Durin::FAssetPath ExpectedPath = Report.PackagePath;
	ASSERT_TRUE(Model.Begin(MakeLevel("Cancel"), Report, true));
	FOperationRecorder Recorder;

	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::Cancel, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Cancelled);
	EXPECT_EQ(Recorder.SaveCount, 0u);
	EXPECT_EQ(Recorder.ActivationCount, 0u);
	EXPECT_EQ(Recorder.UnloadCount, 1u);
	EXPECT_EQ(Recorder.UnloadedPath, ExpectedPath);
	EXPECT_EQ(Recorder.CompletionCount, 1u);
	EXPECT_FALSE(Recorder.bLastCompletionSucceeded);
	EXPECT_FALSE(Model.IsPending());
}

TEST(FAssetStructureUpgradeModelTests, SaveFailureRetainsPendingStateForRetry)
{
	Durin::FAssetStructureUpgradeModel Model;
	ASSERT_TRUE(Model.Begin(
		MakeLevel("SaveFailure"),
		MakeReport(
			"SaveFailure",
			Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
			Durin::Asset::EAssetCompatibilityRisk::None),
		true));
	FOperationRecorder Recorder;
	Recorder.bSaveSucceeds = false;

	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::SaveAndOpen, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::SaveFailed);
	EXPECT_TRUE(Model.IsPending());
	EXPECT_EQ(Recorder.ActivationCount, 0u);
	EXPECT_EQ(Recorder.UnloadCount, 0u);
	EXPECT_EQ(Recorder.CompletionCount, 0u);

	Recorder.bSaveSucceeds = true;
	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::SaveAndOpen, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::Activated);
	EXPECT_EQ(Recorder.CompletionCount, 1u);
	EXPECT_TRUE(Recorder.bLastCompletionSucceeded);
}

TEST(FAssetStructureUpgradeModelTests, ActivationFailureUnloadsAndRejectsDeferredOpen)
{
	Durin::FAssetStructureUpgradeModel Model;
	const Durin::Asset::FAssetLoadReport Report = MakeReport(
		"ActivationFailure",
		Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
		Durin::Asset::EAssetCompatibilityRisk::None);
	const Durin::FAssetPath ExpectedPath = Report.PackagePath;
	ASSERT_TRUE(Model.Begin(MakeLevel("ActivationFailure"), Report, true));
	FOperationRecorder Recorder;
	Recorder.bActivationSucceeds = false;

	EXPECT_EQ(
		Model.Resolve(Durin::EAssetStructureUpgradeDecision::OpenWithoutSaving, Recorder.MakeOperations()),
		Durin::EAssetStructureUpgradeResult::ActivationFailed);
	EXPECT_EQ(Recorder.SaveCount, 0u);
	EXPECT_EQ(Recorder.ActivationCount, 1u);
	EXPECT_EQ(Recorder.UnloadCount, 1u);
	EXPECT_EQ(Recorder.UnloadedPath, ExpectedPath);
	EXPECT_EQ(Recorder.CompletionCount, 1u);
	EXPECT_FALSE(Recorder.bLastCompletionSucceeded);
	EXPECT_FALSE(Model.IsPending());
}
