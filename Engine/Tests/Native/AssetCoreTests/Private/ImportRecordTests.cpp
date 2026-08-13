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
#include "NativeDObjectTestSupport.h"
#include "Threading/RunnableThread.h"

namespace
{
	auto RelocateAssetForTest(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination
	) -> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{
			Source, Destination
		};
		Durin::Asset::FAssetRelocationBatchToken Token;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::AnalyzeAssetRelocationBatch(
				std::span{&Mapping, 1}, Token
			);
		if (Result) Result = Durin::Asset::RevalidateAssetRelocationBatch(Token);
		if (Result) Result = Durin::Asset::ApplyAssetRelocationBatch(Token);
		return Result;
	}

	class FScopedAssetReferenceStore
	{
	public:
		explicit FScopedAssetReferenceStore(
			Durin::Asset::IAssetReferenceStore& Store
		)
			: Handle(Durin::Asset::RegisterAssetReferenceStore(&Store))
		{
		}

		~FScopedAssetReferenceStore()
		{
			Durin::Asset::UnregisterAssetReferenceStore(Handle);
		}

	private:
		Durin::Asset::FAssetReferenceStoreHandle Handle = 0;
	};

	class FImportProgressRecorder final
		: public Durin::Asset::Import::IImportProgressReporter
	{
	public:
		auto Report(const Durin::Asset::Import::FImportProgressEvent& Event) noexcept
			-> void override
		{
			Events.push_back(Event);
		}

		auto Contains(
			Durin::Asset::Import::EImportPhase Phase,
			Durin::Asset::Import::EImportProgressState State
		) const -> bool
		{
			return std::ranges::any_of(Events, [Phase, State](const auto& Event) {
				return Event.Phase == Phase && Event.State == State;
			});
		}

		std::vector<Durin::Asset::Import::FImportProgressEvent> Events;
	};

	class DImportRecordOutputForTest : public Durin::DObject
	{
	public:
		explicit DImportRecordOutputForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get()
		)
			: DObject(Initializer)
		{
		}

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
						Durin::InternalConstructor<DImportRecordOutputForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DImportRecordOutputForTest"
				);
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FInt32PropertyParams ValueProperty = {
				"Value", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DImportRecordOutputForTest, Value))
			};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&ValueProperty
			};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister,
				"Tests::DImportRecordOutputForTest",
				"DImportRecordOutputForTest",
				Properties,
				std::size(Properties)
			};
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
			Durin::Testing::InitializeDObjectSystemForTests();
			Durin::FPaths::SetDerivedDataCacheDirForTests(
				(Durin::Testing::GetTestWorkDirectory() / "ImportRecordDDC").generic_string()
			);
			(void)DImportRecordOutputForTest::StaticClass();
			(void)Durin::Asset::Import::DImportRecord::StaticClass();
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
			.bAuthoringWritable = true
		};
	}

	auto MakePath(std::string_view Text) -> Durin::FAssetPath
	{
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Text, Path));
		return Path;
	}

	class FRecordProvider final : public Durin::Asset::Import::IImportProvider
	{
	public:
		FRecordProvider(std::string InId, std::string InOutputRoot)
			: Id(std::move(InId))
			, OutputRoot(std::move(InOutputRoot))
		{
		}

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> Durin::uint32 override { return 3; }
		auto CanImport(const Durin::Asset::Import::FImportSourceRecognition& Source) const
			-> bool override
		{
			return Source.Extension == ".multi";
		}
		auto CaptureSettings(
			Durin::Asset::Import::FImportPayload& OutSettings,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&
		) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Multi.Settings";
			OutSettings.SchemaVersion = 2;
			OutSettings.Bytes = {0x10, 0x20, 0x30};
			return true;
		}
		auto DiscoverDependencies(
			std::span<const Durin::Asset::Import::FSourceSnapshotEntry>,
			Durin::Asset::Import::FDependencyRequestSink&,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&
		) const -> bool override
		{
			return true;
		}
		auto Plan(
			const Durin::Asset::Import::FSourceSnapshot&,
			const Durin::Asset::Import::FImportPayload&,
			Durin::Asset::Import::FImportPlanBuilder& Builder,
			std::vector<Durin::Asset::Import::FImportDiagnostic>& Diagnostics
		) const -> bool override
		{
			Diagnostics.push_back({.Severity = Durin::Asset::Import::EImportDiagnosticSeverity::Warning, .Category = Durin::Asset::Import::EImportDiagnosticCategory::ProviderFailure, .Identity = "tests.multi.warning", .Phase = "test-plan", .SourceIdentity = "root", .OutputIdentity = "primary", .Message = "Fixture warning accepted by publication."});
			Builder.AddOutput({.StableIdentity = "primary", .Role = "Geometry", .AssetPath = MakePath(OutputRoot + "/Primary"), .AssetClassName = DImportRecordOutputForTest::StaticClass()->GetQualifiedName().ToString(), .Policy = Durin::Asset::Import::EImportOutputPolicy::Create, .Collision = Durin::Asset::Import::EImportCollisionAction::Create, .EstimatedCpuBytes = 64, .EstimatedDiskBytes = 32});
			Builder.AddOutput({.StableIdentity = "peer", .Role = "Metadata", .AssetPath = MakePath(OutputRoot + "/Peer"), .AssetClassName = Durin::DObject::StaticClass()->GetQualifiedName().ToString(), .Policy = Durin::Asset::Import::EImportOutputPolicy::Create, .Collision = Durin::Asset::Import::EImportCollisionAction::Create, .EstimatedCpuBytes = 16, .EstimatedDiskBytes = 8});
			return true;
		}

	private:
		std::string Id;
		std::string OutputRoot;
	};

	class FTestCandidate final : public Durin::Asset::Import::ISingleAssetCandidate
	{
	public:
		FTestCandidate(Durin::DObject* InAsset, bool bInNewAsset, Durin::uint32* InAbandonCount = nullptr)
			: Asset(InAsset)
			, Package(InAsset ? InAsset->GetPackage() : nullptr)
			, bNewAsset(bInNewAsset)
			, AbandonCount(InAbandonCount)
		{
		}

		auto GetAsset() const -> Durin::DObject* override { return Asset; }
		auto GetPackage() const -> Durin::DPackage* override { return Package; }
		auto IsNewAsset() const -> bool override { return bNewAsset; }
		auto GetAuthoredFingerprint() const -> std::string override
		{
			std::string Fingerprint;
			std::string Error;
			if (!Durin::Asset::Import::ComputeImportPackageFingerprint(
					Package, Fingerprint, Error
				)) return {};
			return Fingerprint;
		}
		auto Validate(std::vector<Durin::Asset::Import::FImportDiagnostic>&) const
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

	class FValueExchange final : public Durin::Asset::Import::IPreparedImportedStateExchange
	{
	public:
		FValueExchange(DImportRecordOutputForTest& InTarget, DImportRecordOutputForTest& InCandidate)
			: Target(InTarget)
			, Candidate(InCandidate)
		{
		}
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

	class FNoopExchange final : public Durin::Asset::Import::IPreparedImportedStateExchange
	{
	public:
		auto Commit() noexcept -> void override {}
		auto Reverse() noexcept -> void override {}
		auto Finalize() noexcept -> void override {}
	};

	class FBrokenReverseExchange final
		: public Durin::Asset::Import::IPreparedImportedStateExchange
	{
	public:
		FBrokenReverseExchange(DImportRecordOutputForTest& InTarget, DImportRecordOutputForTest& InCandidate)
			: Target(InTarget)
			, Candidate(InCandidate)
		{
		}
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
		Durin::Asset::Import::FImportPlan GenericPlan;
		Durin::Asset::Import::FImportRecordPayload ProviderState;
	};

	auto BuildScenario(std::string_view Name) -> FScenario
	{
		FScenario Scenario;
		Scenario.Root = Durin::Testing::CreateTestFixtureDirectory(
			std::string("ImportRecord") + std::string(Name)
		);
		Scenario.ProviderId = std::string("Tests.Multi.") + std::string(Name);
		Scenario.OutputRoot = std::string("/ImportRecordTests/") + std::string(Name);
		std::filesystem::create_directories(Scenario.Root / "Content");
		WriteSource(Scenario.Root / "Content" / "Source.multi");

		std::string Error;
		const std::array Bytes = {Durin::uint8{0x41}, Durin::uint8{0x42}};
		EXPECT_TRUE(Durin::Asset::Import::MakeImportRecordPayload(
			"Tests.Multi.ProviderState", 7, Bytes,
			Durin::Asset::Import::MaximumImportRecordProviderStateBytes,
			Scenario.ProviderState, Error
		)) << Error;
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
		ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Register(
			std::make_shared<FRecordProvider>(Scenario.ProviderId, Scenario.OutputRoot),
			Error
		)) << Error;
		const auto Generic = Durin::Asset::Import::CreateImportPlan(
			{.RootSource = {.Path = "/ImportRecordTests/Source.multi"},
			 .ProviderId = Scenario.ProviderId},
			Durin::Asset::Import::GetProviderRegistry()
		);
		ASSERT_TRUE(Generic) << Generic.Message;
		Scenario.GenericPlan = Generic.Plan;
	}

	auto MakeInitialPrepared(const FScenario& Scenario, Durin::int32 PrimaryValue)
		-> Durin::Asset::Import::FPreparedMultiOutputImport
	{
		DImportRecordOutputForTest* Primary = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Scenario.PrimaryPath, Primary));
		if (Primary) Primary->SetValue(PrimaryValue);
		Durin::DObject* Peer = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Scenario.PeerPath, Peer));
		Durin::Asset::Import::FPreparedMultiOutputImport Prepared(
			Scenario.GenericPlan.GetProvider()
		);
		Prepared.Outputs.push_back({.StableIdentity = "primary", .Candidate = std::make_unique<FTestCandidate>(Primary, true)});
		Prepared.Outputs.push_back({.StableIdentity = "peer", .Candidate = std::make_unique<FTestCandidate>(Peer, true)});
		return Prepared;
	}

	auto PlanInitial(FScenario& Scenario, Durin::Asset::Import::FImportRecordIndex& Index)
		-> Durin::Asset::Import::FMultiOutputPlanResult
	{
		return Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ProviderState = Scenario.ProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	}

	auto PublishInitial(
		FScenario& Scenario,
		Durin::Asset::Import::FImportRecordIndex& Index,
		Durin::int32 Value = 11
	) -> Durin::Asset::Import::FMultiOutputExecutionResult
	{
		const auto Plan = PlanInitial(Scenario, Index);
		EXPECT_TRUE(Plan) << Plan.Message;
		if (!Plan) return {};
		return Durin::Asset::Import::ExecuteMultiOutputImport(
			Plan.Plan, MakeInitialPrepared(Scenario, Value), Index
		);
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
		Durin::int32 NewValue
	)
		-> Durin::Asset::Import::FPreparedMultiOutputImport
	{
		DImportRecordOutputForTest* PrimaryCandidate = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(
			MakeTemporaryPath(Scenario.PrimaryPath, "Candidate"), PrimaryCandidate
		));
		if (PrimaryCandidate) PrimaryCandidate->SetValue(NewValue);
		Durin::DObject* PeerCandidate = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(
			MakeTemporaryPath(Scenario.PeerPath, "Candidate"), PeerCandidate
		));

		Durin::Asset::Import::FPreparedMultiOutputImport Prepared(
			Scenario.GenericPlan.GetProvider()
		);
		Prepared.Outputs.push_back({.StableIdentity = "primary", .ExistingTarget = PrimaryTarget, .Candidate = std::make_unique<FTestCandidate>(PrimaryCandidate, false), .Exchange = std::make_unique<FValueExchange>(*PrimaryTarget, *PrimaryCandidate)});
		Prepared.Outputs.push_back({.StableIdentity = "peer", .ExistingTarget = PeerTarget, .Candidate = std::make_unique<FTestCandidate>(PeerCandidate, false), .Exchange = std::make_unique<FNoopExchange>()});
		return Prepared;
	}

	auto HasDiagnostic(
		std::span<const Durin::Asset::Import::FImportRecordIndexDiagnostic> Diagnostics,
		Durin::Asset::Import::EImportRecordIndexDiagnostic Category
	) -> bool
	{
		return std::ranges::any_of(Diagnostics, [Category](const auto& Diagnostic) {
			return Diagnostic.Category == Category;
		});
	}
} // namespace

TEST(FImportRecordFrameworkTests, StructRepairRestoresOutputAndTombstonePaths)
{
	InitializeImportRecordTests();
	const std::array Mounts = {MakeMount(
		Durin::Testing::GetTestWorkDirectory() / "ImportRecordStructRepair"
	)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	std::string Error;
	Durin::FDStructPostDeserializeContext Context{
		.Source = Durin::EDStructDeserializeSource::AuthoredAsset,
		.SourceVersion = 2,
		.Error = &Error
	};

	Durin::Asset::Import::FImportRecordOutput Output;
	Output.AssetPathText = "/ImportRecordTests/Repair/Output";
	auto& OutputOps = Durin::Asset::Import::FImportRecordOutput::StaticStruct()->GetOps();
	ASSERT_NE(OutputOps.PostDeserialize, nullptr);
	ASSERT_TRUE(OutputOps.PostDeserialize(&Output, Context)) << Error;
	EXPECT_EQ(Output.AssetPath.ToString(), Output.AssetPathText);

	Durin::Asset::Import::FImportRecordDetachedTombstone Tombstone;
	Tombstone.LastAssetPathText = "/ImportRecordTests/Repair/Detached";
	auto& TombstoneOps =
		Durin::Asset::Import::FImportRecordDetachedTombstone::StaticStruct()->GetOps();
	ASSERT_NE(TombstoneOps.PostDeserialize, nullptr);
	ASSERT_TRUE(TombstoneOps.PostDeserialize(&Tombstone, Context)) << Error;
	EXPECT_EQ(Tombstone.LastAssetPath.ToString(), Tombstone.LastAssetPathText);

	Output.AssetPathText = "not-an-asset-path";
	Error.clear();
	EXPECT_FALSE(OutputOps.PostDeserialize(&Output, Context));
	EXPECT_FALSE(Error.empty());
}

TEST(FImportRecordFrameworkTests, NamespaceMovePreservesSerializedReflectionIdentity)
{
	InitializeImportRecordTests();
	EXPECT_EQ(
		Durin::Asset::Import::DImportRecord::StaticClass()->GetQualifiedName().ToString(),
		"Durin::AssetImport::DImportRecord");
	EXPECT_EQ(
		Durin::Asset::Import::FImportRecordOutput::StaticStruct()->GetQualifiedName().ToString(),
		"Durin::AssetImport::FImportRecordOutput");
	EXPECT_EQ(
		Z_Construct_DEnum_Durin_Asset_Import_EImportRecordOutputPolicy()->GetQualifiedName().ToString(),
		"Durin::AssetImport::EImportRecordOutputPolicy");
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
	ASSERT_TRUE(Durin::Asset::Import::MakeSiblingImportRecordPath(
		Scenario.PrimaryPath, "Source", DeterministicRecordPath, NamingError
	)) << NamingError;
	EXPECT_EQ(DeterministicRecordPath, Scenario.RecordPath);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 17);
	ASSERT_TRUE(Published) << Published.Message;
	ASSERT_NE(Published.Record, nullptr);
	ASSERT_EQ(Published.Record->GetOutputs().size(), 2u);
	EXPECT_TRUE(Published.Record->IsCookExcluded());
	EXPECT_EQ(Published.Record->GetPrimaryOutput(), Scenario.PrimaryPath);
	ASSERT_EQ(Published.Record->GetAcceptedDiagnostics().size(), 1u);
	EXPECT_EQ(Published.Record->GetAcceptedDiagnostics().front().Identity, "tests.multi.warning");
	const auto WarningComparison = Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ExistingRecord = Published.Record, .ProviderState = Scenario.ProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(WarningComparison) << WarningComparison.Message;
	ASSERT_EQ(WarningComparison.Plan.GetPreview().Warnings.size(), 1u);
	EXPECT_EQ(WarningComparison.Plan.GetPreview().Warnings.front().Change, Durin::Asset::Import::EImportWarningChange::PreviouslyAccepted);
	EXPECT_EQ(WarningComparison.Plan.GetPreview().EstimatedCpuBytes, 80u);
	EXPECT_EQ(WarningComparison.Plan.GetPreview().EstimatedDiskBytes, 40u);

	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Scenario.RecordPath));
	Durin::Asset::Import::DImportRecord* Reloaded = nullptr;
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
	ASSERT_TRUE(RelocateAssetForTest(Scenario.PrimaryPath, MovedPath));
	EXPECT_EQ(Reloaded->GetPrimaryOutput(), Scenario.PrimaryPath);
	EXPECT_TRUE(std::ranges::any_of(Reloaded->GetOutputs(), [&](const auto& Output) {
		return Output.AssetPath == Scenario.PrimaryPath;
	}));
	std::string Error;
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;
	EXPECT_FALSE(HasDiagnostic(Index.GetDiagnostics(), Durin::Asset::Import::EImportRecordIndexDiagnostic::OutputFingerprintMismatch));
}

TEST(FImportRecordFrameworkTests, FixUpRewritesImportRecordDomainPathsInSharedTransaction)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("RedirectorFixup");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 19);
	ASSERT_TRUE(Published) << Published.Message;
	const Durin::FAssetPath MovedPath =
		MakePath(Scenario.OutputRoot + "/PrimaryMoved");
	ASSERT_TRUE(RelocateAssetForTest(Scenario.PrimaryPath, MovedPath));
	ASSERT_EQ(Published.Record->GetPrimaryOutput(), Scenario.PrimaryPath);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation
	));

	{
		FScopedAssetReferenceStore StoreRegistration(Index);
		Durin::Asset::FAssetRedirectorFixupPlan Plan;
		const Durin::Asset::FAssetResult Analysis =
			Durin::Asset::AnalyzeRedirectorFixup(
				std::span{&Scenario.PrimaryPath, 1},
				Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
				Plan
			);
		ASSERT_TRUE(Analysis) << Analysis.Message;
		EXPECT_EQ(Plan.GetStoreOccurrences().size(), 2u);
		const Durin::Asset::FAssetResult Applied =
			Durin::Asset::ApplyRedirectorFixup(Plan);
		ASSERT_TRUE(Applied) << Applied.Message;
	}

	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Scenario.PrimaryPath), nullptr);
	EXPECT_EQ(Published.Record->GetPrimaryOutput(), MovedPath);
	EXPECT_TRUE(std::ranges::any_of(
		Published.Record->GetOutputs(), [&](const auto& Output) {
			return Output.StableIdentity == "primary"
				   && Output.AssetPath == MovedPath;
		}
	));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Scenario.RecordPath));
	Durin::Asset::Import::DImportRecord* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.RecordPath, Reloaded));
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetPrimaryOutput(), MovedPath);
	EXPECT_TRUE(std::ranges::any_of(
		Reloaded->GetOutputs(), [&](const auto& Output) {
			return Output.StableIdentity == "primary"
				   && Output.AssetPath == MovedPath
				   && Output.AssetPathText == MovedPath.ToString();
		}
	));
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(
		Scenario.ProviderId
	));
}

TEST(FImportRecordFrameworkTests, ReimportResolvesMovedManagedOutputWithoutCanonicalizingRecord)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("MovedReimport");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 67);
	ASSERT_TRUE(Published) << Published.Message;
	const Durin::FAssetPath MovedPath =
		MakePath(Scenario.OutputRoot + "/PrimaryMoved");
	ASSERT_TRUE(RelocateAssetForTest(Scenario.PrimaryPath, MovedPath));
	std::string Error;
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;

	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MovedPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	const auto Plan = Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ExistingRecord = Published.Record, .ProviderState = Scenario.ProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto PrimaryEntry = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("primary"),
		&Durin::Asset::Import::FMultiOutputReconciliation::StableIdentity
	);
	ASSERT_NE(PrimaryEntry, Plan.Plan.GetReconciliation().end());
	EXPECT_EQ(PrimaryEntry->AssetPath, Scenario.PrimaryPath);
	EXPECT_EQ(PrimaryEntry->ResolvedAssetPath, MovedPath);
	EXPECT_EQ(PrimaryEntry->ProposedAction, Durin::Asset::Import::EMultiOutputProposedAction::ReplaceManaged);

	const auto Executed = Durin::Asset::Import::ExecuteMultiOutputImport(
		Plan.Plan, PrepareReimport(Scenario, Primary, Peer, 68), Index
	);
	ASSERT_TRUE(Executed) << Executed.Message;
	EXPECT_EQ(Primary->GetValue(), 68);
	const auto StoredPrimary = std::ranges::find(
		Executed.Record->GetOutputs(), std::string("primary"),
		&Durin::Asset::Import::FImportRecordOutput::StableIdentity
	);
	ASSERT_NE(StoredPrimary, Executed.Record->GetOutputs().end());
	EXPECT_EQ(StoredPrimary->AssetPath, Scenario.PrimaryPath);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().ResolveAssetPath(StoredPrimary->AssetPath).FinalPath, MovedPath);
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(
		Scenario.ProviderId
	));
}

TEST(FImportRecordFrameworkTests, RebuildDetectsDuplicateRecordsManagersAndRestartDrift)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("IndexConflict");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 23);
	ASSERT_TRUE(Published) << Published.Message;

	const Durin::Asset::FAssetData* RecordData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(Scenario.RecordPath);
	ASSERT_NE(RecordData, nullptr);
	const std::filesystem::path DuplicatePath =
		Scenario.Root / "Content" / "IndexConflict" / "DuplicateRecord.dasset";
	std::filesystem::create_directories(DuplicatePath.parent_path());
	std::filesystem::copy_file(
		RecordData->PhysicalPath, DuplicatePath,
		std::filesystem::copy_options::overwrite_existing
	);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	std::string Error;
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;
	const auto ConflictDiagnostics = Index.GetDiagnostics();
	EXPECT_TRUE(HasDiagnostic(ConflictDiagnostics, Durin::Asset::Import::EImportRecordIndexDiagnostic::DuplicateRecordId));
	EXPECT_TRUE(HasDiagnostic(ConflictDiagnostics, Durin::Asset::Import::EImportRecordIndexDiagnostic::DuplicateManager));
	const Durin::FAssetPath DuplicateRecordPath =
		MakePath(Scenario.OutputRoot + "/DuplicateRecord");
	Durin::Asset::Import::DImportRecord* DuplicateRecord = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(DuplicateRecordPath, DuplicateRecord));
	ASSERT_NE(DuplicateRecord, nullptr);
	const auto Repaired = Durin::Asset::Import::RepairDuplicatedImportRecord(
		*DuplicateRecord, Index
	);
	ASSERT_TRUE(Repaired) << Repaired.Message;
	EXPECT_TRUE(std::ranges::all_of(DuplicateRecord->GetOutputs(), [](const auto& Output) {
		return Output.Policy == Durin::Asset::Import::EImportRecordOutputPolicy::Detached;
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
	EXPECT_TRUE(HasDiagnostic(Index.GetDiagnostics(), Durin::Asset::Import::EImportRecordIndexDiagnostic::OutputFingerprintMismatch));
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, RejectsStaleTargetWithoutPublishing)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("StaleTarget");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 31);
	ASSERT_TRUE(Published) << Published.Message;
	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	Durin::Asset::Import::FImportRecordPayload NewProviderState;
	const std::array StateBytes = {Durin::uint8{0x50}};
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Import::MakeImportRecordPayload(
		"Tests.Multi.ProviderState", 7, StateBytes,
		Durin::Asset::Import::MaximumImportRecordProviderStateBytes,
		NewProviderState, Error
	));
	const auto Plan = Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ExistingRecord = Published.Record, .ProviderState = NewProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto PriorRecordState = Published.Record->GetState();
	Primary->SetValue(32);
	const auto Result = Durin::Asset::Import::ExecuteMultiOutputImport(
		Plan.Plan, PrepareReimport(Scenario, Primary, Peer, 44), Index
	);
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category, Durin::Asset::Import::EImportDiagnosticCategory::StalePlan);
	EXPECT_EQ(Primary->GetValue(), 32);
	EXPECT_EQ(Published.Record->GetState(), PriorRecordState);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(MakeTemporaryPath(Scenario.PrimaryPath, "Candidate")), nullptr);
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, ReconcilesPersistedPoliciesMissingOutputsAndOrphans)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("PolicyReconciliation");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 41);
	ASSERT_TRUE(Published) << Published.Message;

	auto State = Published.Record->GetState();
	const auto Primary = std::ranges::find(
		State.Outputs, std::string("primary"),
		&Durin::Asset::Import::FImportRecordOutput::StableIdentity
	);
	const auto Peer = std::ranges::find(
		State.Outputs, std::string("peer"),
		&Durin::Asset::Import::FImportRecordOutput::StableIdentity
	);
	ASSERT_NE(Primary, State.Outputs.end());
	ASSERT_NE(Peer, State.Outputs.end());
	Primary->Policy = Durin::Asset::Import::EImportRecordOutputPolicy::Detached;
	Peer->Policy = Durin::Asset::Import::EImportRecordOutputPolicy::Referenced;
	State.Outputs.push_back({.StableIdentity = "retired", .Role = "Legacy", .AssetPath = MakePath(Scenario.OutputRoot + "/Retired"), .AssetClassName = Durin::DObject::StaticClass()->GetQualifiedName().ToString(), .Policy = Durin::Asset::Import::EImportRecordOutputPolicy::Detached});
	std::ranges::sort(State.Outputs, {}, &Durin::Asset::Import::FImportRecordOutput::StableIdentity);
	std::string Error;
	ASSERT_TRUE(Published.Record->SetState(std::move(State), Error)) << Error;
	ASSERT_TRUE(Durin::Asset::SavePackage(Published.Record->GetPackage()));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Scenario.PeerPath));
	Index.NotifyAssetDeleted(Scenario.PeerPath);
	ASSERT_TRUE(Index.Rebuild(Error)) << Error;

	const auto Plan = Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ExistingRecord = Published.Record, .ProviderState = Scenario.ProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto PrimaryEntry = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("primary"),
		&Durin::Asset::Import::FMultiOutputReconciliation::StableIdentity
	);
	const auto PeerEntry = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("peer"),
		&Durin::Asset::Import::FMultiOutputReconciliation::StableIdentity
	);
	ASSERT_NE(PrimaryEntry, Plan.Plan.GetReconciliation().end());
	ASSERT_NE(PeerEntry, Plan.Plan.GetReconciliation().end());
	EXPECT_EQ(PrimaryEntry->ObservedState, Durin::Asset::Import::EMultiOutputObservedState::Detached);
	EXPECT_EQ(PrimaryEntry->ProposedAction, Durin::Asset::Import::EMultiOutputProposedAction::KeepDetached);
	EXPECT_EQ(PeerEntry->ObservedState, Durin::Asset::Import::EMultiOutputObservedState::Missing);
	EXPECT_EQ(PeerEntry->ProposedAction, Durin::Asset::Import::EMultiOutputProposedAction::ReportMissing);
	ASSERT_EQ(Plan.Plan.GetOrphans().size(), 1u);
	EXPECT_EQ(Plan.Plan.GetOrphans().front().ObservedState, Durin::Asset::Import::EMultiOutputObservedState::Orphan);

	const auto Executed = Durin::Asset::Import::ExecuteMultiOutputImport(
		Plan.Plan, {}, Index
	);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_EQ(Executed.Record->GetDetachedTombstones().size(), 1u);
	EXPECT_EQ(Executed.Record->GetDetachedTombstones().front().StableIdentity, "retired");
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
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
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Plan = PlanInitial(Scenario, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Collision = std::ranges::find(
		Plan.Plan.GetReconciliation(), std::string("primary"),
		&Durin::Asset::Import::FMultiOutputReconciliation::StableIdentity
	);
	ASSERT_NE(Collision, Plan.Plan.GetReconciliation().end());
	EXPECT_EQ(Collision->ObservedState, Durin::Asset::Import::EMultiOutputObservedState::Collision);
	EXPECT_EQ(Collision->ProposedAction, Durin::Asset::Import::EMultiOutputProposedAction::RejectCollision);
	EXPECT_TRUE(std::ranges::any_of(Plan.Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category == Durin::Asset::Import::EImportDiagnosticCategory::Collision;
	}));
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(Occupant->GetPackage()));
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, RootLastFailureRestoresPriorRecordAndOutputs)
{
	InitializeImportRecordTests();
	FScenario InitialFailure = BuildScenario("InitialRootFailure");
	const std::array InitialMounts = {MakeMount(InitialFailure.Root)};
	{
		Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(InitialMounts);
		ConfigureScenario(InitialFailure);
		Durin::Asset::Import::FImportRecordIndex Index;
		const auto Plan = PlanInitial(InitialFailure, Index);
		ASSERT_TRUE(Plan) << Plan.Message;
		Durin::Asset::Import::FMultiOutputExecutionOptions Options;
		Options.SaveOptions.ShouldFail = [](
											 Durin::Asset::EAssetBundleSavePhase Phase, size_t
										 ) {
			return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
		};
		const auto Failed = Durin::Asset::Import::ExecuteMultiOutputImport(
			Plan.Plan, MakeInitialPrepared(InitialFailure, 51), Index, Options
		);
		EXPECT_FALSE(Failed);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(InitialFailure.PrimaryPath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(InitialFailure.PeerPath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(InitialFailure.RecordPath), nullptr);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(InitialFailure.PrimaryPath), nullptr);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(InitialFailure.RecordPath), nullptr);
	}
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(
		InitialFailure.ProviderId
	));

	FScenario Scenario = BuildScenario("ReimportRootFailure");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 61);
	ASSERT_TRUE(Published) << Published.Message;
	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	const auto PriorRecordState = Published.Record->GetState();
	std::string PriorOutputFingerprint;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Import::ComputePersistedImportPackageFingerprint(
		Scenario.PrimaryPath, PriorOutputFingerprint, Error
	)) << Error;
	const auto Plan = Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ExistingRecord = Published.Record, .ProviderState = Scenario.ProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	Durin::Asset::Import::FMultiOutputExecutionOptions Options;
	Options.SaveOptions.ShouldFail = [](
										 Durin::Asset::EAssetBundleSavePhase Phase, size_t
									 ) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const auto Failed = Durin::Asset::Import::ExecuteMultiOutputImport(
		Plan.Plan, PrepareReimport(Scenario, Primary, Peer, 77), Index, Options
	);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Primary->GetValue(), 61);
	EXPECT_EQ(Published.Record->GetState(), PriorRecordState);
	std::string CurrentOutputFingerprint;
	ASSERT_TRUE(Durin::Asset::Import::ComputePersistedImportPackageFingerprint(
		Scenario.PrimaryPath, CurrentOutputFingerprint, Error
	)) << Error;
	EXPECT_EQ(CurrentOutputFingerprint, PriorOutputFingerprint);
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, InspectsNavigatesAndDetachesManagedOutput)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("InspectDetach");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 81);
	ASSERT_TRUE(Published) << Published.Message;

	const auto OutputInspection = Durin::Asset::Import::InspectImportRecordForOutput(
		Scenario.PrimaryPath, Index
	);
	ASSERT_TRUE(OutputInspection) << OutputInspection.Message;
	EXPECT_EQ(OutputInspection.RecordPath, Scenario.RecordPath);
	ASSERT_NE(OutputInspection.Record, nullptr);
	EXPECT_EQ(OutputInspection.Outputs.size(), 2u);

	const auto Detached = Durin::Asset::Import::DetachImportRecordOutput(
		*OutputInspection.Record, "primary", Index
	);
	ASSERT_TRUE(Detached) << Detached.Message;
	EXPECT_EQ(Detached.RevealPath, Scenario.PrimaryPath);
	EXPECT_TRUE(Index.FindManagers(Scenario.PrimaryPath).empty());
	const auto RecordInspection = Durin::Asset::Import::InspectImportRecord(
		Scenario.RecordPath, Index
	);
	ASSERT_TRUE(RecordInspection) << RecordInspection.Message;
	ASSERT_EQ(RecordInspection.Outputs.size(), 2u);
	const auto Primary = std::ranges::find(
		RecordInspection.Record->GetOutputs(), std::string("primary"),
		&Durin::Asset::Import::FImportRecordOutput::StableIdentity
	);
	ASSERT_NE(Primary, RecordInspection.Record->GetOutputs().end());
	EXPECT_EQ(Primary->Policy, Durin::Asset::Import::EImportRecordOutputPolicy::Detached);
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
}

TEST(FImportRecordFrameworkTests, AbandonsPreparedCandidatesAndReleasesRetiredProviderLease)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("CandidateLease");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::uint32 AbandonCount = 0;
	Durin::Asset::Import::FProviderLease Provider = Scenario.GenericPlan.GetProvider();
	Scenario.GenericPlan = {};
	{
		DImportRecordOutputForTest* Candidate = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(Scenario.PrimaryPath, Candidate));
		Durin::Asset::Import::FPreparedMultiOutputImport Prepared(Provider);
		Prepared.Outputs.push_back({.StableIdentity = "primary", .Candidate = std::make_unique<FTestCandidate>(Candidate, true, &AbandonCount)});
		Provider = {};
		ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
		EXPECT_FALSE(Durin::Asset::Import::GetProviderRegistry().Find(Scenario.ProviderId));
		EXPECT_GT(Durin::Asset::Import::GetProviderRegistry().GetOutstandingLeaseCount(Scenario.ProviderId), 0u);
	}
	EXPECT_EQ(AbandonCount, 1u);
	EXPECT_EQ(Durin::Asset::Import::GetProviderRegistry().GetOutstandingLeaseCount(Scenario.ProviderId), 0u);
}

TEST(FImportRecordFrameworkTests, ReportsFailedReverseExchangeInvariant)
{
	InitializeImportRecordTests();
	FScenario Scenario = BuildScenario("FailedRestore");
	const std::array Mounts = {MakeMount(Scenario.Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ConfigureScenario(Scenario);
	Durin::Asset::Import::FImportRecordIndex Index;
	const auto Published = PublishInitial(Scenario, Index, 91);
	ASSERT_TRUE(Published) << Published.Message;
	DImportRecordOutputForTest* Primary = nullptr;
	Durin::DObject* Peer = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PrimaryPath, Primary));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Scenario.PeerPath, Peer));
	const auto Plan = Durin::Asset::Import::CreateMultiOutputImportPlan({.GenericPlan = Scenario.GenericPlan, .RecordPath = Scenario.RecordPath, .ExistingRecord = Published.Record, .ProviderState = Scenario.ProviderState, .PrimaryOutput = Scenario.PrimaryPath}, Index);
	ASSERT_TRUE(Plan) << Plan.Message;
	auto Prepared = PrepareReimport(Scenario, Primary, Peer, 123);
	auto* PrimaryCandidate = Cast<DImportRecordOutputForTest>(
		Prepared.Outputs.front().Candidate->GetAsset()
	);
	ASSERT_NE(PrimaryCandidate, nullptr);
	Prepared.Outputs.front().Exchange = std::make_unique<FBrokenReverseExchange>(
		*Primary, *PrimaryCandidate
	);
	Durin::Asset::Import::FMultiOutputExecutionOptions Options;
	FImportProgressRecorder Progress;
	Options.Progress = &Progress;
	Options.SaveOptions.ShouldFail = [](
										 Durin::Asset::EAssetBundleSavePhase Phase, size_t
									 ) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const auto Failed = Durin::Asset::Import::ExecuteMultiOutputImport(
		Plan.Plan, std::move(Prepared), Index, Options
	);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Primary->GetValue(), 123);
	EXPECT_TRUE(std::ranges::any_of(Failed.Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category
			   == Durin::Asset::Import::EImportDiagnosticCategory::RestoreFailure;
	}));
	EXPECT_TRUE(Progress.Contains(
		Durin::Asset::Import::EImportPhase::Validation,
		Durin::Asset::Import::EImportProgressState::Succeeded
	));
	EXPECT_TRUE(Progress.Contains(
		Durin::Asset::Import::EImportPhase::Publication,
		Durin::Asset::Import::EImportProgressState::Failed
	));
	EXPECT_TRUE(Progress.Contains(
		Durin::Asset::Import::EImportPhase::Restore,
		Durin::Asset::Import::EImportProgressState::Failed
	));
	ASSERT_TRUE(Durin::Asset::Import::GetProviderRegistry().Unregister(Scenario.ProviderId));
}
