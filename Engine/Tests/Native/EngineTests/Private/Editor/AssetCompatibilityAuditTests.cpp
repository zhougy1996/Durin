#include "AssetCompatibilityAuditTestSupport.h"

using FAssetCompatibilityAuditTests = FAssetCompatibilityAuditFixture;

TEST_F(FAssetCompatibilityAuditTests, RemainsIdleUntilAnExplicitRunAndSortsPresentationByPath)
{
	std::atomic_uint32_t ProbeCount = 0;
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[&ProbeCount](const auto& Input, const auto&, const auto&) {
			++ProbeCount;
			return MakeCompletedRecord(Input);
		});
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets;
	for (const auto& Data : {MakeData("/AuditTests/Z"), MakeData("/AuditTests/A")})
		Assets.emplace(Data.PackagePath, Data);

	Model.Tick(Assets);
	EXPECT_EQ(ProbeCount, 0u);
	EXPECT_EQ(Model.GetState(), Durin::Editor::EAssetCompatibilityAuditState::Idle);
	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	ASSERT_TRUE(WaitUntil([&] { Model.Tick(Assets); return Model.GetState() != Durin::Editor::EAssetCompatibilityAuditState::Running; }));

	EXPECT_EQ(ProbeCount, 2u);
	EXPECT_EQ(Model.GetState(), Durin::Editor::EAssetCompatibilityAuditState::Completed);
	EXPECT_EQ(Model.GetProgress().Completed, 2u);
	const auto Records = Model.GetPresentationRecords();
	ASSERT_EQ(Records.size(), 2u);
	EXPECT_EQ(Records[0].PackagePath.ToString(), "/AuditTests/A");
	EXPECT_EQ(Records[1].PackagePath.ToString(), "/AuditTests/Z");
}

TEST_F(FAssetCompatibilityAuditTests, CancellationPublishesNoPartialRecordForTheInterruptedPackage)
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
	Model.CancelAndDrain();

	EXPECT_EQ(Model.GetState(), Durin::Editor::EAssetCompatibilityAuditState::Cancelled);
	EXPECT_EQ(Model.GetProgress().Completed, 0u);
	const auto* Record = Model.FindRecord(Data.PackagePath);
	ASSERT_NE(Record, nullptr);
	EXPECT_EQ(Record->Inspection, Durin::EAssetCompatibilityInspection::NotChecked);
}

TEST_F(FAssetCompatibilityAuditTests, IdleTickKeepsPresentationCacheStableUntilCatalogDataChanges)
{
	Durin::Editor::FAssetCompatibilityAuditModel Model;
	const auto Data = MakeData("/AuditTests/Cached");
	const std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{
		{Data.PackagePath, Data}};
	Model.ReconcileAssetCatalog(Assets);

	const auto& FirstPresentation = Model.GetPresentationRecords();
	ASSERT_EQ(FirstPresentation.size(), 1u);
	const auto* FirstStorage = FirstPresentation.data();
	const uint64 FirstRevision = Model.GetPresentationRevision();
	Model.Tick();
	Model.ReconcileAssetCatalog(Assets);

	EXPECT_EQ(Model.GetPresentationRevision(), FirstRevision);
	EXPECT_EQ(Model.GetPresentationRecords().data(), FirstStorage);

	auto Changed = Data;
	++Changed.FileSize;
	Model.ReconcileAssetCatalog({{Changed.PackagePath, Changed}});

	EXPECT_GT(Model.GetPresentationRevision(), FirstRevision);
	const auto& ChangedPresentation = Model.GetPresentationRecords();
	ASSERT_EQ(ChangedPresentation.size(), 1u);
	EXPECT_EQ(ChangedPresentation.front().Fingerprint.FileSize, Changed.FileSize);
}

TEST_F(FAssetCompatibilityAuditTests, ReconciliationIsPathKeyedAndMarksOnlyChangedFingerprintsStale)
{
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[](const auto& Input, const auto&, const auto&) { return MakeCompletedRecord(Input); });
	const auto Kept = MakeData("/AuditTests/Kept", 10, 20);
	const auto Removed = MakeData("/AuditTests/Removed", 30, 40);
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{
		{Kept.PackagePath, Kept}, {Removed.PackagePath, Removed}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	ASSERT_TRUE(WaitUntil([&] { Model.Tick(Assets); return Model.GetState() == Durin::Editor::EAssetCompatibilityAuditState::Completed; }));

	auto Changed = Kept;
	Changed.FileSize = 11;
	const auto Added = MakeData("/AuditTests/Added", 50, 60);
	Assets = {{Changed.PackagePath, Changed}, {Added.PackagePath, Added}};
	Model.Tick(Assets);

	EXPECT_EQ(Model.FindRecord(Removed.PackagePath), nullptr);
	ASSERT_NE(Model.FindRecord(Kept.PackagePath), nullptr);
	EXPECT_EQ(Model.FindRecord(Kept.PackagePath)->Freshness, Durin::EAssetCompatibilityFreshness::Stale);
	ASSERT_NE(Model.FindRecord(Added.PackagePath), nullptr);
	EXPECT_EQ(Model.FindRecord(Added.PackagePath)->Inspection, Durin::EAssetCompatibilityInspection::NotChecked);
}

TEST_F(FAssetCompatibilityAuditTests, RerunAdvancesTheRequestSerialAndReplacesTheLiveIndex)
{
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[](const auto& Input, const auto&, const auto&) { return MakeCompletedRecord(Input); });
	const auto First = MakeData("/AuditTests/First");
	const auto Second = MakeData("/AuditTests/Second");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{{First.PackagePath, First}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	const uint64 FirstSerial = Model.GetRequestSerial();
	ASSERT_TRUE(WaitUntil([&] { Model.Tick(Assets); return Model.GetState() == Durin::Editor::EAssetCompatibilityAuditState::Completed; }));

	Assets = {{Second.PackagePath, Second}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	EXPECT_GT(Model.GetRequestSerial(), FirstSerial);
	EXPECT_EQ(Model.FindRecord(First.PackagePath), nullptr);
	EXPECT_NE(Model.FindRecord(Second.PackagePath), nullptr);
}

TEST_F(FAssetCompatibilityAuditTests, ProjectChangeCancelsAndDrainsBeforeClearingState)
{
	std::atomic_bool Started = false;
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[&Started](const auto&, const auto&, const auto& IsCancelled) {
			Started = true;
			while (!IsCancelled()) std::this_thread::yield();
			return Durin::FAssetPackageCompatibilityProbeResult{
				.Status = Durin::EAssetCompatibilityProbeStatus::Cancelled};
		});
	const auto Data = MakeData("/AuditTests/ProjectSwitch");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{{Data.PackagePath, Data}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	ASSERT_TRUE(WaitUntil([&] { return Started.load(); }));
	const uint64 Serial = Model.GetRequestSerial();

	Model.ProjectChanged();

	EXPECT_EQ(Model.GetState(), Durin::Editor::EAssetCompatibilityAuditState::Idle);
	EXPECT_GT(Model.GetRequestSerial(), Serial);
	EXPECT_TRUE(Model.GetPresentationRecords().empty());
}

TEST_F(FAssetCompatibilityAuditTests, ShutdownClosesAdmissionAndDrainsTheWorker)
{
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[](const auto& Input, const auto&, const auto&) { return MakeCompletedRecord(Input); });
	const auto Data = MakeData("/AuditTests/Shutdown");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{{Data.PackagePath, Data}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));

	Model.Shutdown();

	EXPECT_FALSE(Model.RunAudit(Assets, {}));
}

TEST_F(FAssetCompatibilityAuditTests, PresentationCountsFiltersAndCopiedDiagnosticsUseOrthogonalStates)
{
	auto Compatible = *MakeCompletedRecord({
		.PackagePath = MakePath("/AuditTests/Compatible"),
		.PhysicalPath = "Compatible.dasset",
		.ExpectedFileSize = 10,
		.ExpectedLastWriteTimeTicks = 20}).Record;
	auto Incompatible = Compatible;
	Incompatible.PackagePath = MakePath("/AuditTests/Incompatible");
	Incompatible.Compatibility = Durin::EAssetPackageCompatibility::Incompatible;
	Incompatible.Freshness = Durin::EAssetCompatibilityFreshness::Stale;
	Incompatible.Findings.push_back({
		.Code = Durin::EAssetCompatibilityFindingCode::UnknownField,
		.Diagnostic = "Retired field is present."});
	Durin::FAssetPackageCompatibilityRecord NotChecked{
		.PackagePath = MakePath("/AuditTests/NotChecked"),
		.Inspection = Durin::EAssetCompatibilityInspection::NotChecked};
	const std::array Records{Compatible, Incompatible, NotChecked};

	const auto Counts = Durin::Editor::CountAssetCompatibilityAuditRecords(Records);
	EXPECT_EQ(Counts.Compatible, 1u);
	EXPECT_EQ(Counts.Incompatible, 1u);
	EXPECT_EQ(Counts.Stale, 1u);
	EXPECT_EQ(Counts.NotChecked, 1u);
	EXPECT_TRUE(Durin::Editor::MatchesAssetCompatibilityAuditFilter(
		Incompatible, Durin::Editor::EAssetCompatibilityAuditFilter::Issues));
	EXPECT_TRUE(Durin::Editor::MatchesAssetCompatibilityAuditFilter(
		Incompatible, Durin::Editor::EAssetCompatibilityAuditFilter::Stale));
	EXPECT_FALSE(Durin::Editor::MatchesAssetCompatibilityAuditFilter(
		Compatible, Durin::Editor::EAssetCompatibilityAuditFilter::Issues));
	EXPECT_EQ(
		Durin::Editor::FormatAssetCompatibilityAuditDiagnostics(Incompatible),
		"/AuditTests/Incompatible: Ready / Incompatible / Stale\n[UnknownField] Retired field is present.");
}

TEST_F(FAssetCompatibilityAuditTests, SearchAndReportIncludeFindingsAndCanonicalResaveEvidence)
{
	auto Record = *MakeCompletedRecord({
		.PackagePath = MakePath("/AuditTests/SearchablePackage"),
		.PhysicalPath = "SearchablePackage.dasset",
		.ExpectedFileSize = 10,
		.ExpectedLastWriteTimeTicks = 20}).Record;
	Record.Findings.push_back({
		.Code = Durin::EAssetCompatibilityFindingCode::UnknownField,
		.ObjectPath = "Root.Material",
		.DeclaringType = "Game::DSearchable",
		.FieldName = "LegacyField",
		.Diagnostic = "Retired field is present."});
	Record.CanonicalizationEvidence.push_back({
		.PackagePath = Record.PackagePath,
		.StoredIdentity = "Legacy::DMaterial",
		.CurrentIdentity = "Durin::DMaterial",
		.LogicalPath = "Root.Material"});
	Record.DeprecatedRouteEvidence.push_back({
		.PackagePath = Record.PackagePath,
		.ObjectPath = "Root.Material",
		.DeclaringType = "Game::DSearchable",
		.StoredFieldName = "OldRoughness",
		.DeprecatedPropertyName = "Roughness_DEPRECATED"});

	EXPECT_TRUE(Durin::Editor::MatchesAssetCompatibilityAuditSearch(Record, "searchablepackage"));
	EXPECT_TRUE(Durin::Editor::MatchesAssetCompatibilityAuditSearch(Record, "legacyfield"));
	EXPECT_TRUE(Durin::Editor::MatchesAssetCompatibilityAuditSearch(Record, "LEGACY::DMATERIAL"));
	EXPECT_TRUE(Durin::Editor::MatchesAssetCompatibilityAuditSearch(Record, "oldroughness"));
	EXPECT_FALSE(Durin::Editor::MatchesAssetCompatibilityAuditSearch(Record, "missing diagnostic"));
	EXPECT_EQ(Durin::Editor::FormatAssetCompatibilityAuditReport(std::array{Record}),
		"/AuditTests/SearchablePackage: Ready / Compatible / Current"
		"\n[UnknownField] Retired field is present."
		"\n[CanonicalResaveRecommended] Root.Material: Legacy::DMaterial -> Durin::DMaterial"
		"\n[CanonicalResaveRecommended] Game::DSearchable::OldRoughness uses deprecated route Roughness_DEPRECATED");
}

TEST_F(FAssetCompatibilityAuditTests, StreamsProgressBeforeTypedTerminalPublication)
{
	Durin::FThreadEvent SecondPackageStarted;
	Durin::FThreadEvent ReleaseSecondPackage;
	std::atomic_uint32_t ProbeIndex = 0;
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[&](const auto& Input, const auto&, const auto&) {
			if (ProbeIndex.fetch_add(1) == 1)
			{
				SecondPackageStarted.Trigger();
				ReleaseSecondPackage.Wait();
			}
			return MakeCompletedRecord(Input);
		});
	const auto First = MakeData("/AuditTests/StreamA");
	const auto Second = MakeData("/AuditTests/StreamB");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{
		{First.PackagePath, First}, {Second.PackagePath, Second}};

	ASSERT_TRUE(Model.RunAudit(Assets, {}));
	ASSERT_TRUE(SecondPackageStarted.WaitFor(1.0));
	Model.Tick(Assets);
	EXPECT_EQ(Durin::Editor::EAssetCompatibilityAuditState::Running, Model.GetState());
	EXPECT_EQ(1u, Model.GetProgress().Completed);
	ReleaseSecondPackage.Trigger();
	ASSERT_TRUE(WaitUntil([&] {
		Model.Tick(Assets);
		return Model.GetState() == Durin::Editor::EAssetCompatibilityAuditState::Completed;
	}));
	EXPECT_EQ(2u, Model.GetProgress().Completed);
	EXPECT_EQ(Durin::ETaskTarget::AnyWorker, Model.GetWorkerTaskDiagnostics().Target);
	EXPECT_EQ(Durin::ETaskTarget::GameThreadDeferred, Model.GetTerminalTaskDiagnostics().Target);
}

TEST_F(FAssetCompatibilityAuditTests, ProjectChangeDropsQueuedTerminalPublication)
{
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[](const auto& Input, const auto&, const auto&) { return MakeCompletedRecord(Input); });
	const auto Data = MakeData("/AuditTests/StaleTerminal");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{{Data.PackagePath, Data}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (Durin::GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth == 0
		&& std::chrono::steady_clock::now() < Deadline)
	{
		std::this_thread::yield();
	}
	ASSERT_EQ(1u, Durin::GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth);
	const uint64 Serial = Model.GetRequestSerial();
	Model.ProjectChanged();
	Durin::PumpGameThreadDeferredWork({.bUnlimited = true});

	EXPECT_GT(Model.GetRequestSerial(), Serial);
	EXPECT_EQ(Durin::Editor::EAssetCompatibilityAuditState::Idle, Model.GetState());
	EXPECT_TRUE(Model.GetPresentationRecords().empty());
	EXPECT_EQ(Durin::ETaskState::Canceled, Model.GetTerminalTaskDiagnostics().State);
}

TEST_F(FAssetCompatibilityAuditTests, CrossExecutorShutdownPublishesCurrentTerminalSummary)
{
	Durin::Editor::FAssetCompatibilityAuditModel Model(
		[](const auto& Input, const auto&, const auto&) { return MakeCompletedRecord(Input); });
	const auto Data = MakeData("/AuditTests/ShutdownDrain");
	std::unordered_map<Durin::FPackagePath, Durin::FAssetData> Assets{{Data.PackagePath, Data}};
	ASSERT_TRUE(Model.RunAudit(Assets, {}));

	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);

	EXPECT_EQ(Durin::Editor::EAssetCompatibilityAuditState::Completed, Model.GetState());
	EXPECT_EQ(1u, Model.GetProgress().Completed);
	EXPECT_EQ(Durin::ETaskState::Succeeded, Model.GetTerminalTaskDiagnostics().State);
}
