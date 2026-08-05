#include <gtest/gtest.h>

#include "AssetImportCore.h"
#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "ImportRecord.h"
#include "ImportRecordIndex.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/Paths.h"
#include "MultiOutputImport.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"

namespace
{
	class FImportProgressRecorder final
		: public Durin::AssetImport::IImportProgressReporter
	{
	public:
		auto Report(const Durin::AssetImport::FImportProgressEvent& Event) noexcept
			-> void override
		{
			Events.push_back(Event);
		}

		auto Contains(
			Durin::AssetImport::EImportPhase Phase,
			Durin::AssetImport::EImportProgressState State) const -> bool
		{
			return std::ranges::any_of(Events, [Phase, State](const auto& Event) {
				return Event.Phase == Phase && Event.State == State;
			});
		}

		std::vector<Durin::AssetImport::FImportProgressEvent> Events;
	};

	class DImportRecordOutputForTest : public Durin::DObject
	{
	public:
		explicit DImportRecordOutputForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer) {}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DImportRecordOutputForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor,
					"DImportRecordOutputForTest",
					sizeof(DImportRecordOutputForTest),
					alignof(DImportRecordOutputForTest),
					Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DImportRecordOutputForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DImportRecordOutputForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FPropertyParamsBase ValueProperty = {
				"Value", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DImportRecordOutputForTest, Value)),
				sizeof(Value), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&ValueProperty};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister,
				"Tests::DImportRecordOutputForTest",
				"DImportRecordOutputForTest",
				Properties,
				std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		auto SetValue(Durin::int32 NewValue) -> void
		{
			Value = NewValue;
			MarkPackageDirty();
		}

		auto GetValue() const -> Durin::int32 { return Value; }

	private:
		Durin::int32 Value = 0;
	};

	auto InitializeImportRecordTests() -> void
	{
		static const bool Initialized = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::FNameInit();
			Durin::DObjectInit();
			Durin::FPaths::SetDerivedDataCacheDirForTests(
				(Durin::Testing::GetTestWorkDirectory() / "ImportRecordDDC").generic_string());
			(void)DImportRecordOutputForTest::StaticClass();
			(void)Durin::AssetImport::DImportRecord::StaticClass();
			return true;
		}();
		(void)Initialized;
	}

	auto WriteSource(const std::filesystem::path& Path) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream << "multi-output\n";
		ASSERT_TRUE(Stream.good());
	}

	auto MakeMount(const std::filesystem::path& Root)
		-> Durin::PathUtilities::FMountPoint
	{
		return {
			.VirtualRoot = "/ImportRecordTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true};
	}

	auto MakePath(std::string_view Text) -> Durin::FAssetPath
	{
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Text, Path));
		return Path;
	}

	class FRecordProvider final : public Durin::AssetImport::IImportProvider
	{
	public:
		FRecordProvider(std::string InId, std::string InOutputRoot)
			: Id(std::move(InId)), OutputRoot(std::move(InOutputRoot)) {}

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> Durin::uint32 override { return 3; }
		auto CanImport(const Durin::AssetImport::FImportSourceRecognition& Source) const
			-> bool override
		{
			return Source.Extension == ".multi";
		}
		auto CaptureSettings(
			Durin::AssetImport::FImportPayload& OutSettings,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Multi.Settings";
			OutSettings.SchemaVersion = 2;
			OutSettings.Bytes = {0x10, 0x20, 0x30};
			return true;
		}
		auto DiscoverDependencies(
			std::span<const Durin::AssetImport::FSourceSnapshotEntry>,
			Durin::AssetImport::FDependencyRequestSink&,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			return true;
		}
		auto Plan(
			const Durin::AssetImport::FSourceSnapshot&,
			const Durin::AssetImport::FImportPayload&,
			Durin::AssetImport::FImportPlanBuilder& Builder,
			std::vector<Durin::AssetImport::FImportDiagnostic>& Diagnostics) const -> bool override
		{
			Diagnostics.push_back({
				.Severity = Durin::AssetImport::EImportDiagnosticSeverity::Warning,
				.Category = Durin::AssetImport::EImportDiagnosticCategory::ProviderFailure,
				.Identity = "tests.multi.warning",
				.Phase = "test-plan",
				.SourceIdentity = "root",
				.OutputIdentity = "primary",
				.Message = "Fixture warning accepted by publication."});
			Builder.AddOutput({
				.StableIdentity = "primary",
				.Role = "Geometry",
				.AssetPath = MakePath(OutputRoot + "/Primary"),
				.AssetClassName = DImportRecordOutputForTest::StaticClass()
					->GetQualifiedName().ToString(),
				.Policy = Durin::AssetImport::EImportOutputPolicy::Create,
				.Collision = Durin::AssetImport::EImportCollisionAction::Create,
				.EstimatedCpuBytes = 64,
				.EstimatedDiskBytes = 32});
			Builder.AddOutput({
				.StableIdentity = "peer",
				.Role = "Metadata",
				.AssetPath = MakePath(OutputRoot + "/Peer"),
				.AssetClassName = Durin::DObject::StaticClass()
					->GetQualifiedName().ToString(),
				.Policy = Durin::AssetImport::EImportOutputPolicy::Create,
				.Collision = Durin::AssetImport::EImportCollisionAction::Create,
				.EstimatedCpuBytes = 16,
				.EstimatedDiskBytes = 8});
			return true;
		}

	private:
		std::string Id;
		std::string OutputRoot;
	};

	class FTestCandidate final : public Durin::AssetImport::ISingleAssetCandidate
	{
	public:
		FTestCandidate(Durin::DObject* InAsset, bool bInNewAsset,
			Durin::uint32* InAbandonCount = nullptr)
			: Asset(InAsset), Package(InAsset ? InAsset->GetPackage() : nullptr),
			  bNewAsset(bInNewAsset), AbandonCount(InAbandonCount) {}

		auto GetAsset() const -> Durin::DObject* override { return Asset; }
		auto GetPackage() const -> Durin::DPackage* override { return Package; }
		auto IsNewAsset() const -> bool override { return bNewAsset; }
		auto GetAuthoredFingerprint() const -> std::string override
		{
			std::string Fingerprint;
			std::string Error;
			if (!Durin::AssetImport::ComputeImportPackageFingerprint(
				Package, Fingerprint, Error)) return {};
			return Fingerprint;
		}
		auto Validate(std::vector<Durin::AssetImport::FImportDiagnostic>&) const
			-> bool override
		{
			return Asset && Package;
		}
		auto Abandon() noexcept -> void override
		{
			if (!Package) return;
			if (AbandonCount) ++*AbandonCount;
			(void)Durin::Asset::DiscardUnpublishedPackage(Package);
			Package = nullptr;
			Asset = nullptr;
		}

	private:
		Durin::DObject* Asset = nullptr;
		Durin::DPackage* Package = nullptr;
		bool bNewAsset = false;
		Durin::uint32* AbandonCount = nullptr;
	};

	class FValueExchange final : public Durin::AssetImport::IPreparedImportedStateExchange
	{
	public:
		FValueExchange(DImportRecordOutputForTest& InTarget,
			DImportRecordOutputForTest& InCandidate)
			: Target(InTarget), Candidate(InCandidate) {}
		auto Commit() noexcept -> void override { Swap(); }
		auto Reverse() noexcept -> void override { Swap(); }
		auto Finalize() noexcept -> void override {}

	private:
		auto Swap() noexcept -> void
		{
			const Durin::int32 TargetValue = Target.GetValue();
			Target.SetValue(Candidate.GetValue());
			Candidate.SetValue(TargetValue);
		}
		DImportRecordOutputForTest& Target;
		DImportRecordOutputForTest& Candidate;
	};

	class FNoopExchange final : public Durin::AssetImport::IPreparedImportedStateExchange
	{
	public:
		auto Commit() noexcept -> void override {}
		auto Reverse() noexcept -> void override {}
		auto Finalize() noexcept -> void override {}
	};

	class FBrokenReverseExchange final
		: public Durin::AssetImport::IPreparedImportedStateExchange
	{
	public:
		FBrokenReverseExchange(DImportRecordOutputForTest& InTarget,
			DImportRecordOutputForTest& InCandidate)
			: Target(InTarget), Candidate(InCandidate) {}
		auto Commit() noexcept -> void override { Target.SetValue(Candidate.GetValue()); }
		auto Reverse() noexcept -> void override {}
		auto Finalize() noexcept -> void override {}
	private:
		DImportRecordOutputForTest& Target;
		DImportRecordOutputForTest& Candidate;
	};

	struct FScenario
	{
		std::filesystem::path Root;
		std::string ProviderId;
		std::string OutputRoot;
		Durin::FAssetPath RecordPath;
		Durin::FAssetPath PrimaryPath;
		Durin::FAssetPath PeerPath;
		Durin::AssetImport::FImportPlan GenericPlan;
		Durin::AssetImport::FImportRecordPayload ProviderState;
	};

	auto BuildScenario(std::string_view Name) -> FScenario
	{
		FScenario Scenario;
		Scenario.Root = Durin::Testing::CreateTestFixtureDirectory(
			std::string("ImportRecord") + std::string(Name));
		Scenario.ProviderId = std::string("Tests.Multi.") + std::string(Name);
		Scenario.OutputRoot = std::string("/ImportRecordTests/") + std::string(Name);
		std::filesystem::create_directories(Scenario.Root / "Content");
		WriteSource(Scenario.Root / "Content" / "Source.multi");

		std::string Error;
		const std::array Bytes = {Durin::uint8{0x41}, Durin::uint8{0x42}};
		EXPECT_TRUE(Durin::AssetImport::MakeImportRecordPayload(
			"Tests.Multi.ProviderState", 7, Bytes,
			Durin::AssetImport::MaximumImportRecordProviderStateBytes,
			Scenario.ProviderState, Error)) << Error;
		return Scenario;
	}

	auto ConfigureScenario(FScenario& Scenario) -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Asset::FAssetManager::Get().Initialize();
		Scenario.RecordPath = MakePath(Scenario.OutputRoot + "/Source_Import");
		Scenario.PrimaryPath = MakePath(Scenario.OutputRoot + "/Primary");
		Scenario.PeerPath = MakePath(Scenario.OutputRoot + "/Peer");
		std::string Error;
		ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Register(
			std::make_shared<FRecordProvider>(Scenario.ProviderId, Scenario.OutputRoot),
			Error)) << Error;
		const auto Generic = Durin::AssetImport::CreateImportPlan(
			{
				.RootSource = {.Path = "/ImportRecordTests/Source.multi"},
				.ProviderId = Scenario.ProviderId},
			Durin::AssetImport::GetProviderRegistry());
		ASSERT_TRUE(Generic) << Generic.Message;
		Scenario.GenericPlan = Generic.Plan;
	}

	auto MakeInitialPrepared(const FScenario& Scenario, Durin::int32 PrimaryValue)
		-> Durin::AssetImport::FPreparedMultiOutputImport
	{
		DImportRecordOutputForTest* Primary = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Scenario.PrimaryPath, Primary));
		if (Primary) Primary->SetValue(PrimaryValue);
		Durin::DObject* Peer = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Scenario.PeerPath, Peer));
		Durin::AssetImport::FPreparedMultiOutputImport Prepared(
			Scenario.GenericPlan.GetProvider());
		Prepared.Outputs.push_back({
			.StableIdentity = "primary",
			.Candidate = std::make_unique<FTestCandidate>(Primary, true)});
		Prepared.Outputs.push_back({
			.StableIdentity = "peer",
			.Candidate = std::make_unique<FTestCandidate>(Peer, true)});
		return Prepared;
	}

	auto PlanInitial(FScenario& Scenario, Durin::AssetImport::FImportRecordIndex& Index)
		-> Durin::AssetImport::FMultiOutputPlanResult
	{
		return Durin::AssetImport::CreateMultiOutputImportPlan({
			.GenericPlan = Scenario.GenericPlan,
			.RecordPath = Scenario.RecordPath,
			.ProviderState = Scenario.ProviderState,
			.PrimaryOutput = Scenario.PrimaryPath}, Index);
	}

	auto PublishInitial(
		FScenario& Scenario,
		Durin::AssetImport::FImportRecordIndex& Index,
		Durin::int32 Value = 11) -> Durin::AssetImport::FMultiOutputExecutionResult
	{
		const auto Plan = PlanInitial(Scenario, Index);
		EXPECT_TRUE(Plan) << Plan.Message;
		if (!Plan) return {};
		return Durin::AssetImport::ExecuteMultiOutputImport(
			Plan.Plan, MakeInitialPrepared(Scenario, Value), Index);
	}

	auto MakeTemporaryPath(const Durin::FAssetPath& Base, std::string_view Suffix)
		-> Durin::FAssetPath
	{
		return MakePath(std::format("{}_{}", Base.ToString(), Suffix));
	}

	auto PrepareReimport(
		const FScenario& Scenario,
		DImportRecordOutputForTest* PrimaryTarget,
		Durin::DObject* PeerTarget,
		Durin::int32 NewValue)
		-> Durin::AssetImport::FPreparedMultiOutputImport
	{
		DImportRecordOutputForTest* PrimaryCandidate = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(
			MakeTemporaryPath(Scenario.PrimaryPath, "Candidate"), PrimaryCandidate));
		if (PrimaryCandidate) PrimaryCandidate->SetValue(NewValue);
		Durin::DObject* PeerCandidate = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(
			MakeTemporaryPath(Scenario.PeerPath, "Candidate"), PeerCandidate));

		Durin::AssetImport::FPreparedMultiOutputImport Prepared(
			Scenario.GenericPlan.GetProvider());
		Prepared.Outputs.push_back({
			.StableIdentity = "primary",
			.ExistingTarget = PrimaryTarget,
			.Candidate = std::make_unique<FTestCandidate>(PrimaryCandidate, false),
			.Exchange = std::make_unique<FValueExchange>(
				*PrimaryTarget, *PrimaryCandidate)});
		Prepared.Outputs.push_back({
			.StableIdentity = "peer",
			.ExistingTarget = PeerTarget,
			.Candidate = std::make_unique<FTestCandidate>(PeerCandidate, false),
			.Exchange = std::make_unique<FNoopExchange>()});
		return Prepared;
	}

	auto HasDiagnostic(
		std::span<const Durin::AssetImport::FImportRecordIndexDiagnostic> Diagnostics,
		Durin::AssetImport::EImportRecordIndexDiagnostic Category) -> bool
	{
		return std::ranges::any_of(Diagnostics, [Category](const auto& Diagnostic) {
			return Diagnostic.Category == Category;
		});
	}
}

TEST(FImportRecordFrameworkTests, StructRepairRestoresOutputAndTombstonePaths)
{
	InitializeImportRecordTests();
	const std::array Mounts = {MakeMount(
		Durin::Testing::GetTestWorkDirectory() / "ImportRecordStructRepair")};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	std::string Error;
	Durin::FDStructPostDeserializeContext Context{
		.Source = Durin::EDStructDeserializeSource::AuthoredAsset,
		.SourceVersion = 2,
		.Error = &Error};

	Durin::AssetImport::FImportRecordOutput Output;
	Output.AssetPathText = "/ImportRecordTests/Repair/Output";
	auto& OutputOps = Durin::AssetImport::FImportRecordOutput::StaticStruct()->GetOps();
	ASSERT_NE(OutputOps.PostDeserialize, nullptr);
	ASSERT_TRUE(OutputOps.PostDeserialize(&Output, Context)) << Error;
	EXPECT_EQ(Output.AssetPath.ToString(), Output.AssetPathText);

	Durin::AssetImport::FImportRecordDetachedTombstone Tombstone;
	Tombstone.LastAssetPathText = "/ImportRecordTests/Repair/Detached";
	auto& TombstoneOps =
		Durin::AssetImport::FImportRecordDetachedTombstone::StaticStruct()->GetOps();
	ASSERT_NE(TombstoneOps.PostDeserialize, nullptr);
	ASSERT_TRUE(TombstoneOps.PostDeserialize(&Tombstone, Context)) << Error;
	EXPECT_EQ(Tombstone.LastAssetPath.ToString(), Tombstone.LastAssetPathText);

	Output.AssetPathText = "not-an-asset-path";
	Error.clear();
	EXPECT_FALSE(OutputOps.PostDeserialize(&Output, Context));
	EXPECT_FALSE(Error.empty());
}

TEST(FImportRecordFrameworkTests, PersistsHeterogeneousPeersAcrossReloadMoveAndProviderUnload)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("RoundTripMove");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::FAssetPath DeterministicRecordPath;
	std::string NamingError;
	ASSERT_TRUE(Durin::AssetImport::MakeSiblingImportRecordPath(
		Scenario.PrimaryPath, "Source", DeterministicRecordPath, NamingError)) << NamingError;
	EXPECT_EQ(DeterministicRecordPath, Scenario.RecordPath);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 17);
	ASSERT_TRUE(Published) << Published.Message;
	ASSERT_NE(Published.Record, nullptr);
	ASSERT_EQ(Published.Record->GetOutputs().size(), 2u);
	EXPECT_TRUE(Published.Record->IsCookExcluded());
	EXPECT_EQ(Published.Record->GetPrimaryOutput(), Scenario.PrimaryPath);
	ASSERT_EQ(Published.Record->GetAcceptedDiagnostics().size(), 1u);
	EXPECT_EQ(Published.Record->GetAcceptedDiagnostics().front().Identity,
		"tests.multi.warning");
	const auto WarningComparison = Durin::AssetImport::CreateMultiOutputImportPlan({
		.GenericPlan = Scenario.GenericPlan,
		.RecordPath = Scenario.RecordPath,
		.ExistingRecord = Published.Record,
		.ProviderState = Scenario.ProviderState,
		.PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(WarningComparison) << WarningComparison.Message;
	ASSERT_EQ(WarningComparison.Plan.GetPreview().Warnings.size(), 1u);
	EXPECT_EQ(WarningComparison.Plan.GetPreview().Warnings.front().Change,
		Durin::AssetImport::EImportWarningChange::PreviouslyAccepted);
	EXPECT_EQ(WarningComparison.Plan.GetPreview().EstimatedCpuBytes, 80u);
	EXPECT_EQ(WarningComparison.Plan.GetPreview().EstimatedDiskBytes, 40u);

	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Scenario.RecordPath));
	Durin::AssetImport::DImportRecord* Reloaded = nullptr;
	const Durin::Asset::FAssetResult ReloadResult =
		Durin::Asset::LoadAsset(Scenario.RecordPath, Reloaded);
	ASSERT_TRUE(ReloadResult) << ReloadResult.Message;
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetProviderId(), Scenario.ProviderId);
	EXPECT_EQ(Reloaded->GetProviderState(), Scenario.ProviderState);
	EXPECT_EQ(Reloaded->GetOutputs().size(), 2u);
	for (const auto& Output : Reloaded->GetOutputs())
	{
		EXPECT_TRUE(Output.AssetPath.IsValid());
		EXPECT_EQ(Output.AssetPath.ToString(), Output.AssetPathText);
	}
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Scenario.PeerPath));
	Index.NotifyPackageUnloaded(Scenario.PeerPath);
	EXPECT_EQ(Index.FindRecordOutputs(Scenario.RecordPath).size(), 2u);

	const Durin::FAssetPath MovedPath = MakePath(Scenario.OutputRoot + "/PrimaryMoved");
	ASSERT_TRUE(Durin::Asset::MoveAsset(Scenario.PrimaryPath, MovedPath));
	EXPECT_EQ(Reloaded->GetPrimaryOutput(), MovedPath);
	EXPECT_TRUE(std::ranges::any_of(Reloaded->GetOutputs(), [&](const auto& Output) {
		return Output.AssetPath == MovedPath;
	}));
	std::string Error;
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;
	EXPECT_FALSE(HasDiagnostic(Index.GetDiagnostics(),
		Durin::AssetImport::EImportRecordIndexDiagnostic::OutputFingerprintMismatch));
}

TEST(FImportRecordFrameworkTests, RebuildDetectsDuplicateRecordsManagersAndRestartDrift)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("IndexConflict");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 23);
	ASSERT_TRUE(Published) << Published.Message;

	const Durin::Asset::FAssetData* RecordData =
		Durin::Asset::GetAssetRegistry().FindAsset(Scenario.RecordPath);
	ASSERT_NE(RecordData, nullptr);
	const std::filesystem::path DuplicatePath =
		Scenario.Root / "Content" / "IndexConflict" / "DuplicateRecord.dasset";
	std::filesystem::create_directories(DuplicatePath.parent_path());
	std::filesystem::copy_file(
		RecordData->PhysicalPath, DuplicatePath,
		std::filesystem::copy_options::overwrite_existing);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	std::string Error;
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;
	const auto ConflictDiagnostics = Index.GetDiagnostics();
	EXPECT_TRUE(HasDiagnostic(ConflictDiagnostics,
		Durin::AssetImport::EImportRecordIndexDiagnostic::DuplicateRecordId));
	EXPECT_TRUE(HasDiagnostic(ConflictDiagnostics,
		Durin::AssetImport::EImportRecordIndexDiagnostic::DuplicateManager));
	const Durin::FAssetPath DuplicateRecordPath =
		MakePath(Scenario.OutputRoot + "/DuplicateRecord");
	Durin::AssetImport::DImportRecord* DuplicateRecord = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(DuplicateRecordPath, DuplicateRecord));
	ASSERT_NE(DuplicateRecord, nullptr);
	const auto Repaired = Durin::AssetImport::RepairDuplicatedImportRecord(
		*DuplicateRecord, Index);
	ASSERT_TRUE(Repaired) << Repaired.Message;
	EXPECT_TRUE(std::ranges::all_of(DuplicateRecord->GetOutputs(), [](const auto& Output) {
		return Output.Policy == Durin::AssetImport::EImportRecordOutputPolicy::Detached;
	}));
	EXPECT_FALSE(Index.IsRecordConflicted(Scenario.RecordPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DuplicateRecordPath));

	std::filesystem::remove(DuplicatePath);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	DImportRecordOutputForTest* Output = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Output));
	Output->SetValue(99);
	ASSERT_TRUE(Durin::Asset::SavePackage(Output->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Scenario.PrimaryPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Scenario.RecordPath));
	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::Asset::FAssetManager::Get().Initialize();
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	Index.ClearForProjectSwitch();
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;
	EXPECT_TRUE(HasDiagnostic(Index.GetDiagnostics(),
		Durin::AssetImport::EImportRecordIndexDiagnostic::OutputFingerprintMismatch));
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, RejectsStaleTargetWithoutPublishing)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("StaleTarget");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 31);
	ASSERT_TRUE(Published) << Published.Message;
	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	Durin::AssetImport::FImportRecordPayload NewProviderState;
	const std::array StateBytes = {Durin::uint8{0x50}};
	std::string Error;
	ASSERT_TRUE(Durin::AssetImport::MakeImportRecordPayload(
		"Tests.Multi.ProviderState", 7, StateBytes,
		Durin::AssetImport::MaximumImportRecordProviderStateBytes,
		NewProviderState, Error));
	const auto Plan = Durin::AssetImport::CreateMultiOutputImportPlan({
		.GenericPlan = Scenario.GenericPlan,
		.RecordPath = Scenario.RecordPath,
		.ExistingRecord = Published.Record,
		.ProviderState = NewProviderState,
		.PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto PriorRecordState = Published.Record->GetState();
	Primary->SetValue(32);
	const auto Result = Durin::AssetImport::ExecuteMultiOutputImport(
		Plan.Plan, PrepareReimport(Scenario, Primary, Peer, 44), Index);
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::StalePlan);
	EXPECT_EQ(Primary->GetValue(), 32);
	EXPECT_EQ(Published.Record->GetState(), PriorRecordState);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(
		MakeTemporaryPath(Scenario.PrimaryPath, "Candidate")), nullptr);
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, ReconcilesPersistedPoliciesMissingOutputsAndOrphans)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("PolicyReconciliation");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 41);
	ASSERT_TRUE(Published) << Published.Message;

	auto State = Published.Record->GetState();
	const auto Primary = std::ranges::find(
		State.Outputs, std::string("primary"),
		&Durin::AssetImport::FImportRecordOutput::StableIdentity);
	const auto Peer = std::ranges::find(
		State.Outputs, std::string("peer"),
		&Durin::AssetImport::FImportRecordOutput::StableIdentity);
	ASSERT_NE(Primary, State.Outputs.end());
	ASSERT_NE(Peer, State.Outputs.end());
	Primary->Policy = Durin::AssetImport::EImportRecordOutputPolicy::Detached;
	Peer->Policy = Durin::AssetImport::EImportRecordOutputPolicy::Referenced;
	State.Outputs.push_back({
		.StableIdentity = "retired",
		.Role = "Legacy",
		.AssetPath = MakePath(Scenario.OutputRoot + "/Retired"),
		.AssetClassName = Durin::DObject::StaticClass()->GetQualifiedName().ToString(),
		.Policy = Durin::AssetImport::EImportRecordOutputPolicy::Detached});
	std::ranges::sort(State.Outputs, {},
		&Durin::AssetImport::FImportRecordOutput::StableIdentity);
	std::string Error;
	ASSERT_TRUE(Published.Record->SetState(std::move(State), Error)) << Error;
	ASSERT_TRUE(Durin::Asset::SavePackage(Published.Record->GetPackage()));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Scenario.PeerPath));
	Index.NotifyAssetDeleted(Scenario.PeerPath);
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;

	const auto Plan = Durin::AssetImport::CreateMultiOutputImportPlan({
		.GenericPlan = Scenario.GenericPlan,
		.RecordPath = Scenario.RecordPath,
		.ExistingRecord = Published.Record,
		.ProviderState = Scenario.ProviderState,
		.PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto PrimaryEntry = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("primary"),
		&Durin::AssetImport::FMultiOutputReconciliation::StableIdentity);
	const auto PeerEntry = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("peer"),
		&Durin::AssetImport::FMultiOutputReconciliation::StableIdentity);
	ASSERT_NE(PrimaryEntry, Plan.Plan.GetReconciliation().end());
	ASSERT_NE(PeerEntry, Plan.Plan.GetReconciliation().end());
	EXPECT_EQ(PrimaryEntry->ObservedState,
		Durin::AssetImport::EMultiOutputObservedState::Detached);
	EXPECT_EQ(PrimaryEntry->ProposedAction,
		Durin::AssetImport::EMultiOutputProposedAction::KeepDetached);
	EXPECT_EQ(PeerEntry->ObservedState,
		Durin::AssetImport::EMultiOutputObservedState::Missing);
	EXPECT_EQ(PeerEntry->ProposedAction,
		Durin::AssetImport::EMultiOutputProposedAction::ReportMissing);
	ASSERT_EQ(Plan.Plan.GetOrphans().size(), 1u);
	EXPECT_EQ(Plan.Plan.GetOrphans().front().ObservedState,
		Durin::AssetImport::EMultiOutputObservedState::Orphan);

	const auto Executed = Durin::AssetImport::ExecuteMultiOutputImport(
		Plan.Plan, {}, Index);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_EQ(Executed.Record->GetDetachedTombstones().size(), 1u);
	EXPECT_EQ(Executed.Record->GetDetachedTombstones().front().StableIdentity, "retired");
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, RejectsUnrelatedInitialOutputCollision)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("InitialCollision");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	DImportRecordOutputForTest* Occupant = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Scenario.PrimaryPath, Occupant));
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Plan = PlanInitial(Scenario, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Collision = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("primary"),
		&Durin::AssetImport::FMultiOutputReconciliation::StableIdentity);
	ASSERT_NE(Collision, Plan.Plan.GetReconciliation().end());
	EXPECT_EQ(Collision->ObservedState,
		Durin::AssetImport::EMultiOutputObservedState::Collision);
	EXPECT_EQ(Collision->ProposedAction,
		Durin::AssetImport::EMultiOutputProposedAction::RejectCollision);
	EXPECT_TRUE(std::ranges::any_of(Plan.Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category == Durin::AssetImport::EImportDiagnosticCategory::Collision;
	}));
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(Occupant->GetPackage()));
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, RootLastFailureRestoresPriorRecordAndOutputs)
{
	InitializeImportRecordTests();
	FScenario InitialFailure = BuildScenario("InitialRootFailure");
	const std::array InitialMounts = {MakeMount(InitialFailure.Root)};
	{
		Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(InitialMounts);
		ConfigureScenario(InitialFailure);
		Durin::AssetImport::FImportRecordIndex Index;
		const auto Plan = PlanInitial(InitialFailure, Index);
		ASSERT_TRUE(Plan) << Plan.Message;
		Durin::AssetImport::FMultiOutputExecutionOptions Options;
		Options.SaveOptions.ShouldFail = [](
			Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
			return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
		};
		const auto Failed = Durin::AssetImport::ExecuteMultiOutputImport(
			Plan.Plan, MakeInitialPrepared(InitialFailure, 51), Index, Options);
		EXPECT_FALSE(Failed);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(InitialFailure.PrimaryPath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(InitialFailure.PeerPath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(InitialFailure.RecordPath), nullptr);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(InitialFailure.PrimaryPath), nullptr);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(InitialFailure.RecordPath), nullptr);
	}
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(
		InitialFailure.ProviderId));

	FScenario Scenario = BuildScenario("ReimportRootFailure");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 61);
	ASSERT_TRUE(Published) << Published.Message;
	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	const auto PriorRecordState = Published.Record->GetState();
	std::string PriorOutputFingerprint;
	std::string Error;
	ASSERT_TRUE(Durin::AssetImport::ComputePersistedImportPackageFingerprint(
		Scenario.PrimaryPath, PriorOutputFingerprint, Error)) << Error;
	const auto Plan = Durin::AssetImport::CreateMultiOutputImportPlan({
		.GenericPlan = Scenario.GenericPlan,
		.RecordPath = Scenario.RecordPath,
		.ExistingRecord = Published.Record,
		.ProviderState = Scenario.ProviderState,
		.PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	Durin::AssetImport::FMultiOutputExecutionOptions Options;
	Options.SaveOptions.ShouldFail = [](
		Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const auto Failed = Durin::AssetImport::ExecuteMultiOutputImport(
		Plan.Plan, PrepareReimport(Scenario, Primary, Peer, 77), Index, Options);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Primary->GetValue(), 61);
	EXPECT_EQ(Published.Record->GetState(), PriorRecordState);
	std::string CurrentOutputFingerprint;
	ASSERT_TRUE(Durin::AssetImport::ComputePersistedImportPackageFingerprint(
		Scenario.PrimaryPath, CurrentOutputFingerprint, Error)) << Error;
	EXPECT_EQ(CurrentOutputFingerprint, PriorOutputFingerprint);
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, InspectsNavigatesAndDetachesManagedOutput)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("InspectDetach");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 81);
	ASSERT_TRUE(Published) << Published.Message;

	const auto OutputInspection = Durin::AssetImport::InspectImportRecordForOutput(
		Scenario.PrimaryPath, Index);
	ASSERT_TRUE(OutputInspection) << OutputInspection.Message;
	EXPECT_EQ(OutputInspection.RecordPath, Scenario.RecordPath);
	ASSERT_NE(OutputInspection.Record, nullptr);
	EXPECT_EQ(OutputInspection.Outputs.size(), 2u);

	const auto Detached = Durin::AssetImport::DetachImportRecordOutput(
		*OutputInspection.Record, "primary", Index);
	ASSERT_TRUE(Detached) << Detached.Message;
	EXPECT_EQ(Detached.RevealPath, Scenario.PrimaryPath);
	EXPECT_TRUE(Index.FindManagers(Scenario.PrimaryPath).empty());
	const auto RecordInspection = Durin::AssetImport::InspectImportRecord(
		Scenario.RecordPath, Index);
	ASSERT_TRUE(RecordInspection) << RecordInspection.Message;
	ASSERT_EQ(RecordInspection.Outputs.size(), 2u);
	const auto Primary = std::ranges::find(
		RecordInspection.Record->GetOutputs(), std::string("primary"),
		&Durin::AssetImport::FImportRecordOutput::StableIdentity);
	ASSERT_NE(Primary, RecordInspection.Record->GetOutputs().end());
	EXPECT_EQ(Primary->Policy, Durin::AssetImport::EImportRecordOutputPolicy::Detached);
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, AbandonsPreparedCandidatesAndReleasesRetiredProviderLease)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("CandidateLease");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::uint32 AbandonCount = 0;
	Durin::AssetImport::FProviderLease Provider = Scenario.GenericPlan.GetProvider();
	Scenario.GenericPlan = {};
	{
		DImportRecordOutputForTest* Candidate = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(Scenario.PrimaryPath, Candidate));
		Durin::AssetImport::FPreparedMultiOutputImport Prepared(Provider);
		Prepared.Outputs.push_back({
			.StableIdentity = "primary",
			.Candidate = std::make_unique<FTestCandidate>(Candidate, true, &AbandonCount)});
		Provider = {};
		ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
		EXPECT_FALSE(Durin::AssetImport::GetProviderRegistry().Find(Scenario.ProviderId));
		EXPECT_GT(Durin::AssetImport::GetProviderRegistry().GetOutstandingLeaseCount(
			Scenario.ProviderId), 0u);
	}
	EXPECT_EQ(AbandonCount, 1u);
	EXPECT_EQ(Durin::AssetImport::GetProviderRegistry().GetOutstandingLeaseCount(
		Scenario.ProviderId), 0u);
}

TEST(FImportRecordFrameworkTests, ReportsFailedReverseExchangeInvariant)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("FailedRestore");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::AssetImport::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 91);
	ASSERT_TRUE(Published) << Published.Message;
	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	const auto Plan = Durin::AssetImport::CreateMultiOutputImportPlan({
		.GenericPlan = Scenario.GenericPlan,
		.RecordPath = Scenario.RecordPath,
		.ExistingRecord = Published.Record,
		.ProviderState = Scenario.ProviderState,
		.PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	auto Prepared = PrepareReimport(Scenario, Primary, Peer, 123);
	auto* PrimaryCandidate = Cast<DImportRecordOutputForTest>(
		Prepared.Outputs.front().Candidate->GetAsset());
	ASSERT_NE(PrimaryCandidate, nullptr);
	Prepared.Outputs.front().Exchange = std::make_unique<FBrokenReverseExchange>(
		*Primary, *PrimaryCandidate);
	Durin::AssetImport::FMultiOutputExecutionOptions Options;
	FImportProgressRecorder Progress;
	Options.Progress = &Progress;
	Options.SaveOptions.ShouldFail = [](
		Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const auto Failed = Durin::AssetImport::ExecuteMultiOutputImport(
		Plan.Plan, std::move(Prepared), Index, Options);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Primary->GetValue(), 123);
	EXPECT_TRUE(std::ranges::any_of(Failed.Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category
			== Durin::AssetImport::EImportDiagnosticCategory::RestoreFailure;
	}));
	EXPECT_TRUE(Progress.Contains(
		Durin::AssetImport::EImportPhase::Validation,
		Durin::AssetImport::EImportProgressState::Succeeded));
	EXPECT_TRUE(Progress.Contains(
		Durin::AssetImport::EImportPhase::Publication,
		Durin::AssetImport::EImportProgressState::Failed));
	EXPECT_TRUE(Progress.Contains(
		Durin::AssetImport::EImportPhase::Restore,
		Durin::AssetImport::EImportProgressState::Failed));
	ASSERT_TRUE(Durin::AssetImport::GetProviderRegistry().Unregister(Scenario.ProviderId));
}
