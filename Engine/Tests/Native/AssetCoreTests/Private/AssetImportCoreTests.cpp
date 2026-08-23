#include <gtest/gtest.h>

#include "AssetImportCore.h"
#include "AsyncImport.h"
#include "ImportService.h"
#include "ImportJob.h"
#include "AssetTools.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"

namespace
{
	auto GetImportRegistryTestGate() -> Durin::FModuleOwnedCallbackGate
	{
		static Durin::FModuleTestOwner Context("AssetImportCoreTests.Registry");
		static auto Registration = Context.CreateOwnedCallbackRegistration(
			"AssetImportCoreTests.Registry");
		return Registration.GetGate();
	}

	auto WriteSource(const std::filesystem::path& Path, std::string_view Bytes) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
		ASSERT_TRUE(Stream.good());
	}

	auto MakeMount(const std::filesystem::path& Root) -> Durin::PathUtilities::FMountPoint
	{
		return {
			.VirtualRoot = "/ImportCoreTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true};
	}

	auto Sanitize(std::string Value) -> std::string
	{
		for (char& Character : Value)
			if (!std::isalnum(static_cast<unsigned char>(Character))) Character = '_';
		return Value;
	}

	class FGraphProvider final : public Durin::Asset::IImportProvider
	{
	public:
		explicit FGraphProvider(
			std::string InId = "Tests.Graph", bool bInMatches = true,
			std::shared_ptr<std::atomic_bool> InDestroyed = {})
			: Id(std::move(InId)), bMatches(bInMatches), Destroyed(std::move(InDestroyed)) {}
		~FGraphProvider() override { if (Destroyed) *Destroyed = true; }

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> uint32 override { return 1; }

		auto CanImport(const Durin::Asset::FImportSourceRecognition& Source) const
			-> bool override
		{
			return bMatches && Source.Extension == ".graph"
				&& std::string_view(reinterpret_cast<const char*>(Source.Prefix.data()),
					Source.Prefix.size()).starts_with("graph");
		}

		auto CaptureSettings(
			Durin::Asset::FImportPayload& OutSettings,
			std::vector<Durin::Asset::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Graph.Settings";
			OutSettings.SchemaVersion = 1;
			OutSettings.Bytes = {std::byte{0x47}, std::byte{0x52}, std::byte{0x41},
				std::byte{0x50}, std::byte{0x48}};
			return true;
		}

		auto DiscoverDependencies(
			std::span<const Durin::Asset::FSourceSnapshotEntry> Sources,
			Durin::Asset::FDependencyRequestSink& Sink,
			std::vector<Durin::Asset::FImportDiagnostic>&) const -> bool override
		{
			for (const Durin::Asset::FSourceSnapshotEntry& Source : Sources)
			{
				if (Source.bEmbedded) continue;
				const std::string Text(
					reinterpret_cast<const char*>(Source.GetBytes().data()),
					Source.GetBytes().size());
				std::istringstream Lines(Text);
				std::string Line;
				while (std::getline(Lines, Line))
				{
					std::istringstream Fields(Line);
					std::string Kind;
					std::string Identity;
					std::string Value;
					Fields >> Kind >> Identity >> Value;
					if (Kind == "dep")
					{
						if (!Sink.AddRelative(Source.StableIdentity, Identity, "Dependency", Value))
							return false;
					}
					else if (Kind == "optional")
					{
						if (!Sink.AddRelative(Source.StableIdentity, Identity, "Optional", Value, true))
							return false;
					}
					else if (Kind == "embedded")
					{
					const auto Bytes = std::as_bytes(std::span{Value});
						if (!Sink.AddEmbedded(Source.StableIdentity, Identity, "Embedded", Bytes))
							return false;
					}
				}
			}
			return true;
		}

		auto Plan(
			const Durin::Asset::FSourceSnapshot& Snapshot,
			const Durin::Asset::FImportPayload&,
			Durin::Asset::FImportPlanBuilder& Builder,
			std::vector<Durin::Asset::FImportDiagnostic>&) const -> bool override
		{
			for (auto It = Snapshot.GetSources().rbegin(); It != Snapshot.GetSources().rend(); ++It)
			{
				Durin::FAssetPath Path;
				const std::string PathText =
					"/ImportCoreTests/Planned/" + Sanitize(It->StableIdentity);
				if (!Durin::FAssetPath::TryCreate(PathText, Path)) return false;
				Builder.AddOutput({
					.StableIdentity = It->StableIdentity,
					.Role = It->Role,
					.AssetPath = std::move(Path),
					.AssetClassName = "Tests.GraphAsset",
					.Policy = Durin::Asset::EImportOutputPolicy::Create,
					.Collision = Durin::Asset::EImportCollisionAction::Create,
					.EstimatedCpuBytes = It->ByteCount,
					.EstimatedDiskBytes = It->ByteCount});
			}
			Builder.SetProviderData(std::make_shared<const uint32>(17));
			return true;
		}

	private:
		std::string Id;
		bool bMatches = true;
		std::shared_ptr<std::atomic_bool> Destroyed;
	};

	class FTaskSchedulerGuard
	{
	public:
		~FTaskSchedulerGuard()
		{
			Durin::Asset::GetImportService().CancelAndDrainAllAsyncImports();
			Durin::ShutdownTaskScheduler(false);
		}
	};

	struct FBlockingProviderState
	{
		std::mutex Mutex;
		std::condition_variable Condition;
		bool bEntered = false;
		bool bRelease = false;
		bool bObservedWorkerPreparation = false;
	};

	class FBlockingGraphProvider final : public Durin::Asset::IImportProvider
	{
	public:
		FBlockingGraphProvider(std::string InId,
			std::shared_ptr<FBlockingProviderState> InState)
			: Id(std::move(InId)), State(std::move(InState)) {}

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> uint32 override { return 1; }
		auto CanImport(const Durin::Asset::FImportSourceRecognition& Source) const
			-> bool override { return Source.Extension == ".graph"; }
		auto CaptureSettings(Durin::Asset::FImportPayload& OutSettings,
			std::vector<Durin::Asset::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Blocking.Settings";
			OutSettings.SchemaVersion = 1;
			OutSettings.Bytes = {std::byte{1}};
			return true;
		}
		auto DiscoverDependencies(
			std::span<const Durin::Asset::FSourceSnapshotEntry>,
			Durin::Asset::FDependencyRequestSink&,
			std::vector<Durin::Asset::FImportDiagnostic>&) const -> bool override
		{
			return true;
		}
		auto Plan(const Durin::Asset::FSourceSnapshot&,
			const Durin::Asset::FImportPayload&,
			Durin::Asset::FImportPlanBuilder& Builder,
			std::vector<Durin::Asset::FImportDiagnostic>&) const -> bool override
		{
			{
				std::lock_guard Lock(State->Mutex);
				State->bEntered = true;
				State->bObservedWorkerPreparation =
					Durin::Asset::IsImportWorkerPreparation();
			}
			State->Condition.notify_all();
			std::unique_lock Lock(State->Mutex);
			while (!State->bRelease
				&& !Durin::Asset::IsImportCancellationRequested())
				State->Condition.wait_for(Lock, std::chrono::milliseconds(1));
			if (Durin::Asset::IsImportCancellationRequested()) return false;
			Durin::FAssetPath Path;
			if (!Durin::FAssetPath::TryCreate("/ImportCoreTests/Planned/blocking", Path))
				return false;
			Builder.AddOutput({
				.StableIdentity = "blocking",
				.Role = "Blocking",
				.AssetPath = std::move(Path),
				.AssetClassName = "Tests.GraphAsset"});
			return true;
		}

	private:
		std::string Id;
		std::shared_ptr<FBlockingProviderState> State;
	};

	auto WaitForAsyncResult(
		const Durin::Asset::FAsyncImportPlanHandle& Handle,
		Durin::Asset::FImportPlanResult& OutResult)
		-> Durin::Asset::EAsyncImportPlanStatus
	{
		for (uint32 Attempt = 0; Attempt < 5'000; ++Attempt)
		{
			(void)Durin::Asset::DrainAsyncImportCompletionMailbox();
			const auto Status = Durin::Asset::TryTakeAsyncImportPlanResult(
				Handle, OutResult);
			if (Status != Durin::Asset::EAsyncImportPlanStatus::Pending)
				return Status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Durin::Asset::EAsyncImportPlanStatus::Pending;
	}

	auto WaitForDetachedResult(
		Durin::Asset::FAsyncImportExecutionHandle& Handle,
		Durin::Asset::FDetachedImportBuildResult& OutResult)
		-> Durin::Asset::EAsyncImportPlanStatus
	{
		for (uint32 Attempt = 0; Attempt < 5'000; ++Attempt)
		{
			const auto Status = Durin::Asset::PollAsyncImportExecution(
				Handle, OutResult);
			if (Status != Durin::Asset::EAsyncImportPlanStatus::Pending)
				return Status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Durin::Asset::EAsyncImportPlanStatus::Pending;
	}

	class FProgressRecorder final : public Durin::Asset::IImportProgressReporter
	{
	public:
		auto Report(const Durin::Asset::FImportProgressEvent& Event) noexcept
			-> void override
		{
			Events.push_back(Event);
		}
		std::vector<Durin::Asset::FImportProgressEvent> Events;
	};

	auto RegisterGraphProvider(
		Durin::Asset::FImportService& Registry,
		std::string_view Id = "Tests.Graph") -> void
	{
		std::string Error;
		ASSERT_TRUE(Registry.RegisterImporter({
			.Provider = std::make_shared<FGraphProvider>(std::string(Id))},
			GetImportRegistryTestGate(), Error)) << Error;
	}
}

TEST(FAssetImportCoreTests, ImportServiceRegistersOneDescriptorAtomically)
{
	Durin::Asset::FImportService Service;
	std::string Error;
	ASSERT_TRUE(Service.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>("Tests.Descriptor")},
		GetImportRegistryTestGate(), Error)) << Error;
	EXPECT_TRUE(Service.IsImporterRegistered("Tests.Descriptor"));

	EXPECT_FALSE(Service.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>("Tests.Descriptor")},
		GetImportRegistryTestGate(), Error));
	EXPECT_TRUE(Service.IsImporterRegistered("Tests.Descriptor"));
	EXPECT_TRUE(Service.UnregisterImporter("Tests.Descriptor"));
	EXPECT_FALSE(Service.IsImporterRegistered("Tests.Descriptor"));
	EXPECT_FALSE(Service.UnregisterImporter("Tests.Descriptor"));
}

TEST(FAssetImportCoreTests, ScopedImporterCollisionCannotRemoveForeignRegistration)
{
	Durin::Asset::FImportService Service;
	std::string Error;
	auto Existing = Service.RegisterImporterScoped({
		.Provider = std::make_shared<FGraphProvider>("Tests.ScopedCollision")},
		GetImportRegistryTestGate(), Error);
	ASSERT_TRUE(Existing) << Error;

	auto Collision = Service.RegisterImporterScoped({
		.Provider = std::make_shared<FGraphProvider>("Tests.ScopedCollision")},
		GetImportRegistryTestGate(), Error);
	EXPECT_FALSE(Collision);
	EXPECT_TRUE(Service.IsImporterRegistered("Tests.ScopedCollision"));
	EXPECT_TRUE(Service.FindImporter("Tests.ScopedCollision"));
	EXPECT_TRUE(Existing.Reset());
	EXPECT_FALSE(Service.IsImporterRegistered("Tests.ScopedCollision"));
}

TEST(FAssetImportCoreTests, StaleScopedImporterCannotRemoveReusedProviderId)
{
	Durin::Asset::FImportService Service;
	std::string Error;
	auto Stale = Service.RegisterImporterScoped({
		.Provider = std::make_shared<FGraphProvider>("Tests.ScopedReuse")},
		GetImportRegistryTestGate(), Error);
	ASSERT_TRUE(Stale) << Error;
	ASSERT_TRUE(Service.UnregisterImporter("Tests.ScopedReuse"));

	auto Current = Service.RegisterImporterScoped({
		.Provider = std::make_shared<FGraphProvider>("Tests.ScopedReuse")},
		GetImportRegistryTestGate(), Error);
	ASSERT_TRUE(Current) << Error;
	EXPECT_FALSE(Stale.Reset());
	EXPECT_TRUE(Service.IsImporterRegistered("Tests.ScopedReuse"));
	EXPECT_TRUE(Service.FindImporter("Tests.ScopedReuse"));
	EXPECT_TRUE(Current.Reset());
}

TEST(FAssetImportCoreTests, ScopedImporterHandleMayOutliveService)
{
	Durin::Asset::FImporterRegistration Registration;
	{
		Durin::Asset::FImportService Service;
		std::string Error;
		Registration = Service.RegisterImporterScoped({
			.Provider = std::make_shared<FGraphProvider>("Tests.ScopedLifetime")},
			GetImportRegistryTestGate(), Error);
		ASSERT_TRUE(Registration) << Error;
	}
	EXPECT_FALSE(Registration.Reset());
}

TEST(FAssetImportCoreTests, ProviderOwnerRetirementRejectsLookupAndAuditsLeaseDestruction)
{
	Durin::FModuleTestOwner Context("AssetImportCoreTests.ProviderRetirement");
	auto Registration = Context.CreateOwnedCallbackRegistration(
		"AssetImportCore.ProviderRegistry");
	Durin::Asset::FImportService Registry;
	auto Destroyed = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>("Tests.Retirement", true, Destroyed)},
		Registration.GetGate(), Error)) << Error;
	auto Lease = Registry.FindImporter("Tests.Retirement");
	ASSERT_TRUE(Lease);
	const auto Retiring = Registration.Retire();
	// One lease represents registry storage and one represents the escaped provider.
	EXPECT_EQ(2u, Retiring.RetainedResourceCount);
	EXPECT_FALSE(Registry.FindImporter("Tests.Retirement"));
	EXPECT_TRUE(Registry.UnregisterImporter("Tests.Retirement"));
	EXPECT_FALSE(Destroyed->load());
	EXPECT_EQ(Durin::EModularFeatureRetirementStatus::TimedOut,
		Registration.Reset(std::chrono::milliseconds(1)).Status);
	Lease = {};
	EXPECT_TRUE(Destroyed->load());
	EXPECT_TRUE(Registration.Reset().Succeeded());
}

TEST(FAssetImportCoreTests, CapturedBytesRemainImmutableAfterPhysicalSourceChanges)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreImmutable");
	WriteSource(Root / "Content" / "Root.graph", "graph\nembedded payload original\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;
	RegisterGraphProvider(Registry);

	Durin::Asset::FSourceSnapshotBuilder Builder;
	std::vector<Durin::Asset::FImportDiagnostic> Diagnostics;
	ASSERT_TRUE(Builder.CaptureRoot({.Path = "/ImportCoreTests/Root.graph"}, Diagnostics));
	WriteSource(Root / "Content" / "Root.graph", "graph\nchanged after capture\n");
	ASSERT_TRUE(Builder.DiscoverDependencies(Registry.FindImporter("Tests.Graph"), Diagnostics));
	const std::shared_ptr<const Durin::Asset::FSourceSnapshot> Snapshot =
		Builder.Freeze(Diagnostics);
	ASSERT_NE(Snapshot, nullptr);
	const Durin::Asset::FSourceSnapshotEntry* Captured = Snapshot->FindSource("root");
	ASSERT_NE(Captured, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Captured->GetBytes().data()),
		Captured->GetBytes().size()), "graph\nembedded payload original\n");
	const Durin::Asset::FSourceSnapshotEntry* Embedded = Snapshot->FindSource("payload");
	ASSERT_NE(Embedded, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Embedded->GetBytes().data()),
		Embedded->GetBytes().size()), "original");
	Durin::Asset::FImportPayload Settings;
	ASSERT_TRUE(Registry.FindImporter("Tests.Graph").GetProvider()->CaptureSettings(
		Settings, Diagnostics));
	std::string SettingsError;
	ASSERT_TRUE(Settings.Finalize(SettingsError)) << SettingsError;
	const auto Plan = Durin::Asset::BuildImportPlan(
		Registry.FindImporter("Tests.Graph"), Snapshot, Settings, Registry.GetImporterRevision());
	ASSERT_TRUE(Plan) << Plan.Message;
	EXPECT_EQ(Plan.Plan.GetSnapshot().FindSource("root")->ContentHash,
		Durin::FXxHash128::HashBuffer(Captured->GetBytes()));
}

TEST(FAssetImportCoreTests, RejectsTraversalAndRequiredMissingDependencies)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreUnsafe");
	WriteSource(Root / "Content" / "Traversal.graph", "graph\ndep escape ../Outside.bin\n");
	WriteSource(Root / "Content" / "Missing.graph", "graph\ndep absent Missing.bin\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;
	RegisterGraphProvider(Registry);

	const Durin::Asset::FImportPlanResult Traversal = Registry.CreateImportPlan(
		{{.Path = "/ImportCoreTests/Traversal.graph"}});
	ASSERT_FALSE(Traversal);
	EXPECT_EQ(Traversal.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::UnsafeDependency);

	const Durin::Asset::FImportPlanResult Missing = Registry.CreateImportPlan(
		{{.Path = "/ImportCoreTests/Missing.graph"}});
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::MissingDependency);
}

TEST(FAssetImportCoreTests, HandlesOptionalDuplicateAndCyclicDependenciesCanonically)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreClosure");
	WriteSource(Root / "Content" / "Root.graph",
		"graph\ndep child Child.graph\ndep child Child.graph\noptional absent Missing.bin\n");
	WriteSource(Root / "Content" / "Child.graph", "graph\ndep root Root.graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;
	RegisterGraphProvider(Registry);

	const Durin::Asset::FImportPlanResult Result = Registry.CreateImportPlan(
		{{.Path = "/ImportCoreTests/Root.graph"}});
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Plan.GetSnapshot().GetSources().size(), 2u);
	EXPECT_EQ(Result.Plan.GetSnapshot().GetSources()[0].StableIdentity, "child");
	EXPECT_EQ(Result.Plan.GetSnapshot().GetSources()[1].StableIdentity, "root");
	EXPECT_TRUE(std::ranges::any_of(Result.Diagnostics,
		[](const Durin::Asset::FImportDiagnostic& Diagnostic) {
			return Diagnostic.Category
				== Durin::Asset::EImportDiagnosticCategory::MissingDependency
				&& Diagnostic.Severity
					== Durin::Asset::EImportDiagnosticSeverity::Warning;
		}));
}

TEST(FAssetImportCoreTests, EnforcesSourceCountByteAndSettingsBudgets)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreBudgets");
	WriteSource(Root / "Content" / "Root.graph", "graph\ndep child Child.graph\n");
	WriteSource(Root / "Content" / "Child.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;
	RegisterGraphProvider(Registry);

	Durin::Asset::FImportPlanRequest CountRequest{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	CountRequest.Limits.MaximumSourceCount = 1;
	const auto CountResult = Registry.CreateImportPlan(CountRequest);
	ASSERT_FALSE(CountResult);
	EXPECT_EQ(CountResult.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::Asset::FImportPlanRequest ByteRequest = CountRequest;
	ByteRequest.Limits.MaximumSourceCount = 8;
	ByteRequest.Limits.MaximumBytesPerSource = 4;
	const auto ByteResult = Registry.CreateImportPlan(ByteRequest);
	ASSERT_FALSE(ByteResult);
	EXPECT_EQ(ByteResult.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::Asset::FImportPlanRequest SettingsRequest = CountRequest;
	SettingsRequest.Limits.MaximumSourceCount = 8;
	SettingsRequest.Limits.MaximumSettingsBytes = 2;
	const auto SettingsResult = Registry.CreateImportPlan(SettingsRequest);
	ASSERT_FALSE(SettingsResult);
	EXPECT_EQ(SettingsResult.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::Asset::FImportPlanRequest DepthRequest = CountRequest;
	DepthRequest.Limits.MaximumSourceCount = 8;
	DepthRequest.Limits.MaximumDependencyDepth = 0;
	const auto DepthResult = Registry.CreateImportPlan(DepthRequest);
	ASSERT_FALSE(DepthResult);
	EXPECT_EQ(DepthResult.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ResourceLimitExceeded);

	WriteSource(Root / "Content" / "Embedded.graph", "graph\nembedded payload bytes\n");
	Durin::Asset::FImportPlanRequest EmbeddedRequest{
		.RootSource = {.Path = "/ImportCoreTests/Embedded.graph"}};
	EmbeddedRequest.Limits.MaximumEmbeddedBytes = 2;
	const auto EmbeddedResult = Registry.CreateImportPlan(EmbeddedRequest);
	ASSERT_FALSE(EmbeddedResult);
	EXPECT_EQ(EmbeddedResult.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ResourceLimitExceeded);
}

TEST(FAssetImportCoreTests, ProducesDeterministicMutationFreePlans)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreDeterministic");
	WriteSource(Root / "Content" / "Root.graph",
		"graph\ndep beta B.graph\ndep alpha A.graph\nembedded inline bytes\n");
	WriteSource(Root / "Content" / "A.graph", "graph\n");
	WriteSource(Root / "Content" / "B.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;
	RegisterGraphProvider(Registry);
	const uint64 RegistryRevision = Durin::Asset::GetAssetCatalogRevision();

	const Durin::Asset::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	const auto First = Registry.CreateImportPlan(Request);
	const auto Second = Registry.CreateImportPlan(Request);
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;
	EXPECT_EQ(First.Plan.GetFingerprint(), Second.Plan.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		First.Plan.GetOutputs(), Second.Plan.GetOutputs()));
	EXPECT_EQ(First.Plan.GetSnapshot().GetSources().size(), 4u);
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), RegistryRevision);
	for (const Durin::Asset::FImportOutputPreview& Output : First.Plan.GetOutputs())
	{
		EXPECT_EQ(Durin::Asset::FindResidentPackage(Output.AssetPath), nullptr);
		EXPECT_FALSE(std::filesystem::exists(
			Root / "Content" / (std::string(Output.AssetPath.GetAssetName()) + ".dasset")));
	}
}

TEST(FAssetImportCoreTests, ReportsProviderAbsenceAmbiguityAndRetainsLeases)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreProviders");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;

	const Durin::Asset::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	const auto Absent = Registry.CreateImportPlan(Request);
	ASSERT_FALSE(Absent);
	EXPECT_EQ(Absent.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ProviderUnavailable);

	RegisterGraphProvider(Registry, "Tests.First");
	RegisterGraphProvider(Registry, "Tests.Second");
	const auto Ambiguous = Registry.CreateImportPlan(Request);
	ASSERT_FALSE(Ambiguous);
	EXPECT_EQ(Ambiguous.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ProviderAmbiguous);

	Durin::Asset::FProviderLease Lease = Registry.FindImporter("Tests.First");
	ASSERT_TRUE(Lease);
	ASSERT_TRUE(Registry.UnregisterImporter("Tests.First"));
	EXPECT_TRUE(Lease);
	EXPECT_EQ(Lease.GetProviderId(), "Tests.First");
	EXPECT_FALSE(Registry.FindImporter("Tests.First"));

	Durin::Asset::FImportPlanRequest Explicit = Request;
	Explicit.ProviderId = "Tests.First";
	const auto Unregistered = Registry.CreateImportPlan(Explicit);
	ASSERT_FALSE(Unregistered);
	EXPECT_EQ(Unregistered.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ProviderUnavailable);
}

TEST(FAssetImportCoreTests, ReportsSynchronousPhaseBoundariesAndDiagnosticContext)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreProgress");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FImportService Registry;
	RegisterGraphProvider(Registry);
	FProgressRecorder Progress;
	const auto Planned = Registry.CreateImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.Progress = &Progress});
	ASSERT_TRUE(Planned) << Planned.Message;
	const std::array Expected = {
		std::pair{Durin::Asset::EImportPhase::Snapshot,
			Durin::Asset::EImportProgressState::Started},
		std::pair{Durin::Asset::EImportPhase::Snapshot,
			Durin::Asset::EImportProgressState::Succeeded},
		std::pair{Durin::Asset::EImportPhase::Parse,
			Durin::Asset::EImportProgressState::Started},
		std::pair{Durin::Asset::EImportPhase::Parse,
			Durin::Asset::EImportProgressState::Succeeded},
		std::pair{Durin::Asset::EImportPhase::Plan,
			Durin::Asset::EImportProgressState::Started},
		std::pair{Durin::Asset::EImportPhase::Plan,
			Durin::Asset::EImportProgressState::Succeeded}};
	ASSERT_EQ(Progress.Events.size(), Expected.size());
	for (size_t Index = 0; Index < Expected.size(); ++Index)
	{
		EXPECT_EQ(Progress.Events[Index].Phase, Expected[Index].first);
		EXPECT_EQ(Progress.Events[Index].State, Expected[Index].second);
		EXPECT_FALSE(Progress.Events[Index].SourceIdentity.empty());
		EXPECT_FALSE(Progress.Events[Index].OutputIdentity.empty());
	}

	FProgressRecorder FailureProgress;
	Durin::Asset::FImportService EmptyRegistry;
	const auto Failed = EmptyRegistry.CreateImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.Progress = &FailureProgress});
	ASSERT_FALSE(Failed);
	ASSERT_FALSE(Failed.Diagnostics.empty());
	for (const auto& Diagnostic : Failed.Diagnostics)
	{
		EXPECT_FALSE(Diagnostic.Identity.empty());
		EXPECT_FALSE(Diagnostic.Phase.empty());
		EXPECT_FALSE(Diagnostic.SourceIdentity.empty());
		EXPECT_FALSE(Diagnostic.OutputIdentity.empty());
	}
	ASSERT_EQ(FailureProgress.Events.size(), 2u);
	EXPECT_EQ(FailureProgress.Events.back().State,
		Durin::Asset::EImportProgressState::Failed);
}

TEST(FAssetImportCoreTests, AsyncPreparationMatchesSynchronousPlan)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreAsyncEquivalence");
	WriteSource(Root / "Content" / "Root.graph",
		"graph\ndep child Child.graph\nembedded inline bytes\n");
	WriteSource(Root / "Content" / "Child.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.AsyncEquivalence";
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId)},
		GetImportRegistryTestGate(), Error)) << Error;

	Durin::Asset::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId};
	Durin::Asset::FImportPlanResult Synchronous =
		Registry.CreateImportPlan(Request);
	ASSERT_TRUE(Synchronous) << Synchronous.Message;
	const Durin::Asset::FAsyncImportPlanHandle Handle =
		Durin::Asset::GetImportService().LaunchAsyncImportPlan(
			Request, "Tests.AsyncEquivalence.Owner");
	ASSERT_TRUE(Handle);
	Durin::Asset::FImportPlanResult Asynchronous;
	ASSERT_EQ(WaitForAsyncResult(Handle, Asynchronous),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Asynchronous) << Asynchronous.Message;
	EXPECT_EQ(Synchronous.Plan.GetFingerprint(), Asynchronous.Plan.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		Synchronous.Plan.GetOutputs(), Asynchronous.Plan.GetOutputs()));
	EXPECT_EQ(Synchronous.Diagnostics, Asynchronous.Diagnostics);
	EXPECT_EQ(Durin::Asset::DrainAsyncImportCompletionMailbox(), 0u);
	Durin::Asset::FImportPlanResult SecondTake;
	SecondTake.Message = "unchanged";
	EXPECT_EQ(Durin::Asset::TryTakeAsyncImportPlanResult(Handle, SecondTake),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	EXPECT_EQ(SecondTake.Message, "unchanged");

	const Durin::FTaskSchedulerDiagnostics TaskDiagnostics = Durin::GetTaskSchedulerDiagnostics();
	auto FindTaskOwnerCategory = [&TaskDiagnostics](std::string_view Category) -> const Durin::FTaskOwnerCategoryDiagnostics* {
		const auto Iterator = std::ranges::find_if(TaskDiagnostics.OwnerCategoryDiagnostics, [Category](const Durin::FTaskOwnerCategoryDiagnostics& Entry) {
			return Entry.Owner == "AssetImport" && Entry.Category == Category;
		});
		return Iterator == TaskDiagnostics.OwnerCategoryDiagnostics.end() ? nullptr : &*Iterator;
	};
	const Durin::FTaskOwnerCategoryDiagnostics* PrepareDiagnostics = FindTaskOwnerCategory("PreparePlan");
	const Durin::FTaskOwnerCategoryDiagnostics* PublishDiagnostics = FindTaskOwnerCategory("PublishPlan");
	ASSERT_NE(PrepareDiagnostics, nullptr);
	ASSERT_NE(PublishDiagnostics, nullptr);
	EXPECT_EQ(PrepareDiagnostics->AcceptedCount, 1u);
	EXPECT_EQ(PrepareDiagnostics->SucceededCount, 1u);
	EXPECT_EQ(PrepareDiagnostics->CurrentNonterminalCount, 0u);
	EXPECT_EQ(PrepareDiagnostics->CurrentRetainedUniqueResultBytes, 0u);
	EXPECT_EQ(PublishDiagnostics->AcceptedCount, 1u);
	EXPECT_EQ(PublishDiagnostics->SucceededCount, 1u);
	EXPECT_EQ(PublishDiagnostics->CurrentNonterminalCount, 0u);
	EXPECT_EQ(TaskDiagnostics.LiveScopeCount, 1u);
	EXPECT_EQ(TaskDiagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(TaskDiagnostics.NonquiescentScopeCount, 0u);

	Synchronous = {};
	Asynchronous = {};
	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
}

TEST(FAssetImportCoreTests, NewOwnerSerialSupersedesOlderMailboxResult)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreAsyncSerial");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.AsyncSerial";
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId)},
		GetImportRegistryTestGate(), Error)) << Error;
	const Durin::Asset::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId};
	const auto First = Durin::Asset::GetImportService().LaunchAsyncImportPlan(
		Request, "Tests.AsyncSerial.Owner");
	const auto Second = Durin::Asset::GetImportService().LaunchAsyncImportPlan(
		Request, "Tests.AsyncSerial.Owner");
	ASSERT_LT(First.GetSerial(), Second.GetSerial());
	Durin::Asset::FImportPlanResult FirstResult;
	EXPECT_EQ(WaitForAsyncResult(First, FirstResult),
		Durin::Asset::EAsyncImportPlanStatus::Superseded);
	Durin::Asset::FImportPlanResult SecondResult;
	EXPECT_EQ(WaitForAsyncResult(Second, SecondResult),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	EXPECT_TRUE(SecondResult);
	SecondResult = {};
	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
}

TEST(FAssetImportCoreTests, ImportOperationContractRejectsInvalidTransitions)
{
	using Durin::Asset::EImportOperationState;
	using Durin::Asset::IsImportOperationTransitionAllowed;

	EXPECT_TRUE(IsImportOperationTransitionAllowed(
		EImportOperationState::Queued, EImportOperationState::Running));
	EXPECT_TRUE(IsImportOperationTransitionAllowed(
		EImportOperationState::Running, EImportOperationState::Finalizing));
	EXPECT_TRUE(IsImportOperationTransitionAllowed(
		EImportOperationState::Finalizing, EImportOperationState::Succeeded));
	EXPECT_TRUE(IsImportOperationTransitionAllowed(
		EImportOperationState::Running, EImportOperationState::Canceling));
	EXPECT_TRUE(IsImportOperationTransitionAllowed(
		EImportOperationState::Canceling, EImportOperationState::Canceled));
	EXPECT_FALSE(IsImportOperationTransitionAllowed(
		EImportOperationState::Canceling, EImportOperationState::Finalizing));
	EXPECT_FALSE(IsImportOperationTransitionAllowed(
		EImportOperationState::Succeeded, EImportOperationState::Failed));
	EXPECT_FALSE(IsImportOperationTransitionAllowed(
		EImportOperationState::Running, EImportOperationState::Running));
}

TEST(FAssetImportCoreTests, ImportJobValuesAreOwnedAndEditorAdvancesAreExplicit)
{
	struct FTrackedValue final : Durin::Asset::IImportJobValue
	{
		explicit FTrackedValue(bool& InDestroyed) : Destroyed(InDestroyed) {}
		~FTrackedValue() override { Destroyed = true; }
		bool& Destroyed;
	};

	static_assert(std::has_virtual_destructor_v<Durin::Asset::IImportJobValue>);
	static_assert(!std::is_copy_constructible_v<Durin::Asset::FImportJobWorkerStep>);
	bool bDestroyed = false;
	Durin::Asset::FImportJobWorkerStep Step{
		.Name = "Tests.TypedWorkerStep",
		.Input = std::make_unique<FTrackedValue>(bDestroyed)};
	auto Advance = Durin::Asset::FImportJobEditorAdvance::ContinueWith(std::move(Step));
	EXPECT_TRUE(Advance.IsValid());
	EXPECT_FALSE(bDestroyed);
	Advance.Worker.reset();
	EXPECT_TRUE(bDestroyed);
	EXPECT_FALSE(Advance.IsValid());

	auto Terminal = Durin::Asset::FImportJobEditorAdvance::Complete({
		.State = Durin::Asset::EImportOperationState::Succeeded});
	EXPECT_TRUE(Terminal.IsValid());
}

TEST(FAssetImportCoreTests, ServiceOwnedJobAlternatesWorkerAndEditorStepsWithoutDomainPolling)
{
	struct FRunState
	{
		std::vector<uint32> EditorValues;
		std::atomic<uint32> WorkerSteps = 0;
		std::atomic<bool> bJobDestroyed = false;
		std::atomic<bool> bWorkersMarked = true;
	};
	struct FValue final : Durin::Asset::IImportJobValue
	{
		explicit FValue(uint32 InValue) : Value(InValue) {}
		uint32 Value = 0;
	};
	class FAlternatingJob final : public Durin::Asset::IImportJob
	{
	public:
		explicit FAlternatingJob(std::shared_ptr<FRunState> InRunState)
			: RunState(std::move(InRunState))
		{
			Owner.OwnerId = "Tests.JobKernel.Alternating";
			Owner.ConflictIdentities = {"asset:/Tests/Alternating"};
		}
		~FAlternatingJob() override { RunState->bJobDestroyed.store(true); }
		auto GetProviderId() const -> std::string_view override { return {}; }
		auto GetOwner() const -> const Durin::Asset::FImportOperationOwner& override
		{
			return Owner;
		}
		auto GetLifetime() const -> Durin::Asset::EImportOperationLifetime override
		{
			return Durin::Asset::EImportOperationLifetime::EditorOperation;
		}
		auto AdvanceOnEditor(
			Durin::Asset::FImportJobEditorContext& Context,
			std::unique_ptr<Durin::Asset::IImportJobValue> Previous)
			-> Durin::Asset::FImportJobEditorAdvance override
		{
			if (Stage == 0)
			{
				++Stage;
				return Durin::Asset::FImportJobEditorAdvance::ContinueWith({
					.Name = "AssetImportTests.Alternating.First",
					.Attribution = Durin::RegisterTaskAttribution(
						"AssetImportTests", "JobKernel"),
					.Input = std::make_unique<FValue>(2)});
			}
			const auto* Value = dynamic_cast<const FValue*>(Previous.get());
			if (!Value)
				return Durin::Asset::FImportJobEditorAdvance::Complete({
					.State = Durin::Asset::EImportOperationState::Failed,
					.Diagnostic = "Worker result type mismatch."});
			RunState->EditorValues.push_back(Value->Value);
			if (Stage++ == 1)
				return Durin::Asset::FImportJobEditorAdvance::ContinueWith({
					.Name = "AssetImportTests.Alternating.Second",
					.Attribution = Durin::RegisterTaskAttribution(
						"AssetImportTests", "JobKernel"),
					.Input = std::make_unique<FValue>(3)});
			if (!Context.EnterFinalization())
				return Durin::Asset::FImportJobEditorAdvance::Complete({
					.State = Durin::Asset::EImportOperationState::Canceled,
					.Diagnostic = "Finalization was canceled."});
			return Durin::Asset::FImportJobEditorAdvance::Complete({
				.State = Durin::Asset::EImportOperationState::Succeeded,
				.PublishedAssetIdentities = {"asset:/Tests/Alternating"},
				.RevealIdentity = "asset:/Tests/Alternating"});
		}
		auto ExecuteWorkerStep(
			Durin::Asset::FImportJobWorkerContext&,
			std::unique_ptr<Durin::Asset::IImportJobValue> Input)
			-> Durin::Asset::FImportJobWorkerResult override
		{
			RunState->WorkerSteps.fetch_add(1);
			RunState->bWorkersMarked.store(
				RunState->bWorkersMarked.load() && Durin::Asset::IsImportWorkerPreparation());
			const auto* Value = dynamic_cast<const FValue*>(Input.get());
			if (!Value) return {.bSucceeded = false, .Diagnostic = "Input type mismatch."};
			return {.Value = std::make_unique<FValue>(Value->Value * 2)};
		}
	private:
		std::shared_ptr<FRunState> RunState;
		Durin::Asset::FImportOperationOwner Owner;
		uint32 Stage = 0;
	};

	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const auto RunState = std::make_shared<FRunState>();
	const Durin::Asset::FImportOperationHandle Operation =
		Durin::Asset::GetImportService().SubmitImportJob(
			std::make_unique<FAlternatingJob>(RunState), "Alternating import job");
	ASSERT_TRUE(Operation);
	EXPECT_TRUE(Durin::Asset::GetImportService().HasActiveImportClaim(
		"asset:/Tests/Alternating"));
	for (uint32 Attempt = 0; Attempt < 10'000 && !Operation.GetSnapshot().IsTerminal(); ++Attempt)
	{
		(void)Durin::Asset::GetImportService().PumpImportOperations();
		std::this_thread::yield();
	}
	ASSERT_TRUE(Operation.GetSnapshot().IsTerminal());
	EXPECT_EQ(Operation.GetSnapshot().State,
		Durin::Asset::EImportOperationState::Succeeded);
	EXPECT_EQ(RunState->WorkerSteps.load(), 2u);
	EXPECT_TRUE(RunState->bWorkersMarked.load());
	EXPECT_EQ(RunState->EditorValues, (std::vector<uint32>{4, 6}));
	EXPECT_TRUE(RunState->bJobDestroyed.load());
	EXPECT_FALSE(Durin::Asset::GetImportService().HasActiveImportClaim(
		"asset:/Tests/Alternating"));
	Durin::Asset::FImportOutcome Outcome;
	ASSERT_TRUE(Operation.TryGetOutcome(Outcome));
	EXPECT_EQ(Outcome.PublishedAssetIdentities,
		(std::vector<std::string>{"asset:/Tests/Alternating"}));
	const auto InlineState = std::make_shared<FRunState>();
	const Durin::Asset::FImportOutcome InlineOutcome =
		Durin::Asset::GetImportService().RunImportJobInline(
			std::make_unique<FAlternatingJob>(InlineState), "Inline alternating import job");
	EXPECT_EQ(InlineOutcome, Outcome);
	EXPECT_EQ(InlineState->WorkerSteps.load(), RunState->WorkerSteps.load());
	EXPECT_EQ(InlineState->EditorValues, RunState->EditorValues);
	EXPECT_TRUE(InlineState->bWorkersMarked.load());
	EXPECT_TRUE(InlineState->bJobDestroyed.load());
}

TEST(FAssetImportCoreTests, JobKernelHandlesSupersessionAbandonedHandlesAndAdmissionFailures)
{
	class FKernelProbeJob final : public Durin::Asset::IImportJob
	{
	public:
		FKernelProbeJob(std::string OwnerId,
			Durin::Asset::EImportOperationLifetime InLifetime,
			uint64 InEstimatedBytes,
			std::shared_ptr<std::atomic_bool> InDestroyed)
			: Lifetime(InLifetime), EstimatedBytes(InEstimatedBytes),
			  Destroyed(std::move(InDestroyed))
		{
			Owner.OwnerId = std::move(OwnerId);
		}
		~FKernelProbeJob() override { Destroyed->store(true); }
		auto GetProviderId() const -> std::string_view override { return {}; }
		auto GetOwner() const -> const Durin::Asset::FImportOperationOwner& override
		{
			return Owner;
		}
		auto GetLifetime() const -> Durin::Asset::EImportOperationLifetime override
		{
			return Lifetime;
		}
		auto AdvanceOnEditor(
			Durin::Asset::FImportJobEditorContext&,
			std::unique_ptr<Durin::Asset::IImportJobValue>)
			-> Durin::Asset::FImportJobEditorAdvance override
		{
			if (bWorkerSubmitted)
				return Durin::Asset::FImportJobEditorAdvance::Complete({
					.State = Durin::Asset::EImportOperationState::Succeeded});
			bWorkerSubmitted = true;
			return Durin::Asset::FImportJobEditorAdvance::ContinueWith({
				.Name = "AssetImportTests.KernelProbe",
				.EstimatedResultBytes = EstimatedBytes});
		}
		auto ExecuteWorkerStep(
			Durin::Asset::FImportJobWorkerContext&,
			std::unique_ptr<Durin::Asset::IImportJobValue>)
			-> Durin::Asset::FImportJobWorkerResult override
		{
			return {};
		}
	private:
		Durin::Asset::FImportOperationOwner Owner;
		Durin::Asset::EImportOperationLifetime Lifetime;
		uint64 EstimatedBytes = 0;
		std::shared_ptr<std::atomic_bool> Destroyed;
		bool bWorkerSubmitted = false;
	};

	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	auto& Service = Durin::Asset::GetImportService();
	const auto FirstDestroyed = std::make_shared<std::atomic_bool>(false);
	const auto SecondDestroyed = std::make_shared<std::atomic_bool>(false);
	const auto First = Service.SubmitImportJob(std::make_unique<FKernelProbeJob>(
		"Tests.Preview.Owner", Durin::Asset::EImportOperationLifetime::EphemeralPreview,
		64, FirstDestroyed), "First preview");
	const auto Second = Service.SubmitImportJob(std::make_unique<FKernelProbeJob>(
		"Tests.Preview.Owner", Durin::Asset::EImportOperationLifetime::EphemeralPreview,
		64, SecondDestroyed), "Second preview");
	EXPECT_EQ(First.GetSnapshot().State,
		Durin::Asset::EImportOperationState::Superseded);
	EXPECT_TRUE(FirstDestroyed->load());
	for (uint32 Attempt = 0; Attempt < 10'000 && !Second.GetSnapshot().IsTerminal(); ++Attempt)
	{
		(void)Service.PumpImportOperations();
		std::this_thread::yield();
	}
	EXPECT_EQ(Second.GetSnapshot().State,
		Durin::Asset::EImportOperationState::Succeeded);

	const auto OversizedDestroyed = std::make_shared<std::atomic_bool>(false);
	const auto Oversized = Service.SubmitImportJob(std::make_unique<FKernelProbeJob>(
		"Tests.Oversized", Durin::Asset::EImportOperationLifetime::EditorOperation,
		Durin::Asset::MaximumImportJobDetachedValueBytes + 1, OversizedDestroyed),
		"Oversized job");
	(void)Service.PumpImportOperations();
	EXPECT_EQ(Oversized.GetSnapshot().State,
		Durin::Asset::EImportOperationState::Rejected);
	EXPECT_TRUE(OversizedDestroyed->load());

	const auto AbandonedDestroyed = std::make_shared<std::atomic_bool>(false);
	{
		auto Abandoned = Service.SubmitImportJob(std::make_unique<FKernelProbeJob>(
			"Tests.Abandoned", Durin::Asset::EImportOperationLifetime::EditorOperation,
			64, AbandonedDestroyed), "Abandoned handle job");
		ASSERT_TRUE(Abandoned);
	}
	for (uint32 Attempt = 0; Attempt < 10'000 && !AbandonedDestroyed->load(); ++Attempt)
	{
		(void)Service.PumpImportOperations();
		std::this_thread::yield();
	}
	EXPECT_TRUE(AbandonedDestroyed->load());

	Durin::ShutdownTaskScheduler(false);
	const auto RejectedDestroyed = std::make_shared<std::atomic_bool>(false);
	const auto Rejected = Service.SubmitImportJob(std::make_unique<FKernelProbeJob>(
		"Tests.ScheduleRejected", Durin::Asset::EImportOperationLifetime::EditorOperation,
		64, RejectedDestroyed), "Rejected job");
	(void)Service.PumpImportOperations();
	EXPECT_EQ(Rejected.GetSnapshot().State,
		Durin::Asset::EImportOperationState::Rejected);
	EXPECT_TRUE(RejectedDestroyed->load());
}

TEST(FAssetImportCoreTests, WorkerFailureReturnsToEditorForCompensationWithInlineParity)
{
	struct FCompensationState
	{
		bool bProvisionalMutation = false;
		bool bCompensated = false;
	};
	class FCompensatingJob final : public Durin::Asset::IImportJob
	{
	public:
		explicit FCompensatingJob(std::shared_ptr<FCompensationState> InState)
			: State(std::move(InState)) { Owner.OwnerId = "Tests.Compensation"; }
		auto GetProviderId() const -> std::string_view override { return {}; }
		auto GetOwner() const -> const Durin::Asset::FImportOperationOwner& override
		{
			return Owner;
		}
		auto GetLifetime() const -> Durin::Asset::EImportOperationLifetime override
		{
			return Durin::Asset::EImportOperationLifetime::EditorOperation;
		}
		auto AdvanceOnEditor(
			Durin::Asset::FImportJobEditorContext&,
			std::unique_ptr<Durin::Asset::IImportJobValue>)
			-> Durin::Asset::FImportJobEditorAdvance override
		{
			State->bProvisionalMutation = true;
			return Durin::Asset::FImportJobEditorAdvance::ContinueWith({
				.Name = "AssetImportTests.CompensationFailure"});
		}
		auto ExecuteWorkerStep(
			Durin::Asset::FImportJobWorkerContext&,
			std::unique_ptr<Durin::Asset::IImportJobValue>)
			-> Durin::Asset::FImportJobWorkerResult override
		{
			return {.bSucceeded = false, .Diagnostic = "Injected worker failure."};
		}
		auto CompensateWorkerFailureOnEditor(
			Durin::Asset::FImportJobEditorContext&,
			Durin::Asset::FImportJobWorkerResult Result)
			-> Durin::Asset::FImportOutcome override
		{
			if (Durin::Asset::IsImportWorkerPreparation())
				return {.State = Durin::Asset::EImportOperationState::Failed,
					.Diagnostic = "Compensation ran on a worker."};
			State->bProvisionalMutation = false;
			State->bCompensated = true;
			return {.State = Durin::Asset::EImportOperationState::Failed,
				.Diagnostic = std::move(Result.Diagnostic)};
		}
	private:
		std::shared_ptr<FCompensationState> State;
		Durin::Asset::FImportOperationOwner Owner;
	};

	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	auto& Service = Durin::Asset::GetImportService();
	const auto ScheduledState = std::make_shared<FCompensationState>();
	const auto Operation = Service.SubmitImportJob(
		std::make_unique<FCompensatingJob>(ScheduledState), "Compensating job");
	for (uint32 Attempt = 0; Attempt < 10'000 && !Operation.GetSnapshot().IsTerminal(); ++Attempt)
	{
		(void)Service.PumpImportOperations();
		std::this_thread::yield();
	}
	Durin::Asset::FImportOutcome ScheduledOutcome;
	ASSERT_TRUE(Operation.TryGetOutcome(ScheduledOutcome));
	EXPECT_EQ(ScheduledOutcome.State, Durin::Asset::EImportOperationState::Failed);
	EXPECT_TRUE(ScheduledState->bCompensated);
	EXPECT_FALSE(ScheduledState->bProvisionalMutation);

	const auto InlineState = std::make_shared<FCompensationState>();
	const auto InlineOutcome = Service.RunImportJobInline(
		std::make_unique<FCompensatingJob>(InlineState), "Inline compensating job");
	EXPECT_EQ(InlineOutcome, ScheduledOutcome);
	EXPECT_TRUE(InlineState->bCompensated);
	EXPECT_FALSE(InlineState->bProvisionalMutation);
}

TEST(FAssetImportCoreTests, ProviderDrainDestroysJobValuesBeforeReleasingItsLease)
{
	struct FTrackedValue final : Durin::Asset::IImportJobValue
	{
		explicit FTrackedValue(std::shared_ptr<std::atomic_bool> InDestroyed)
			: Destroyed(std::move(InDestroyed)) {}
		~FTrackedValue() override { Destroyed->store(true); }
		std::shared_ptr<std::atomic_bool> Destroyed;
	};
	class FBlockingJob final : public Durin::Asset::IImportJob
	{
	public:
		FBlockingJob(std::string InProviderId,
			std::shared_ptr<std::atomic_bool> InValueDestroyed,
			std::shared_ptr<std::atomic_bool> InJobDestroyed,
			std::shared_ptr<std::atomic_bool> InEntered)
			: ProviderId(std::move(InProviderId)), ValueDestroyed(std::move(InValueDestroyed)),
			  JobDestroyed(std::move(InJobDestroyed)), Entered(std::move(InEntered))
		{
			Owner.OwnerId = "Tests.JobKernel.ProviderDrain";
		}
		~FBlockingJob() override { JobDestroyed->store(true); }
		auto GetProviderId() const -> std::string_view override { return ProviderId; }
		auto GetOwner() const -> const Durin::Asset::FImportOperationOwner& override
		{
			return Owner;
		}
		auto GetLifetime() const -> Durin::Asset::EImportOperationLifetime override
		{
			return Durin::Asset::EImportOperationLifetime::EditorOperation;
		}
		auto AdvanceOnEditor(
			Durin::Asset::FImportJobEditorContext&,
			std::unique_ptr<Durin::Asset::IImportJobValue>)
			-> Durin::Asset::FImportJobEditorAdvance override
		{
			return Durin::Asset::FImportJobEditorAdvance::ContinueWith({
				.Name = "AssetImportTests.ProviderDrain",
				.Input = std::make_unique<FTrackedValue>(ValueDestroyed)});
		}
		auto ExecuteWorkerStep(
			Durin::Asset::FImportJobWorkerContext& Context,
			std::unique_ptr<Durin::Asset::IImportJobValue> Input)
			-> Durin::Asset::FImportJobWorkerResult override
		{
			Entered->store(true);
			while (!Context.Cancellation.IsCancellationRequested()) std::this_thread::yield();
			return {.bSucceeded = false, .bCanceled = true, .Value = std::move(Input)};
		}
	private:
		std::string ProviderId;
		Durin::Asset::FImportOperationOwner Owner;
		std::shared_ptr<std::atomic_bool> ValueDestroyed;
		std::shared_ptr<std::atomic_bool> JobDestroyed;
		std::shared_ptr<std::atomic_bool> Entered;
	};

	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	auto& Service = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.JobKernel.Provider";
	const auto ProviderDestroyed = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	ASSERT_TRUE(Service.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId, true, ProviderDestroyed)},
		GetImportRegistryTestGate(), Error)) << Error;
	const auto ValueDestroyed = std::make_shared<std::atomic_bool>(false);
	const auto JobDestroyed = std::make_shared<std::atomic_bool>(false);
	const auto Entered = std::make_shared<std::atomic_bool>(false);
	const auto Operation = Service.SubmitImportJob(std::make_unique<FBlockingJob>(
		ProviderId, ValueDestroyed, JobDestroyed, Entered), "Provider drain job");
	ASSERT_TRUE(Operation);
	(void)Service.PumpImportOperations();
	for (uint32 Attempt = 0; Attempt < 10'000 && !Entered->load(); ++Attempt)
		std::this_thread::yield();
	ASSERT_TRUE(Entered->load());
	EXPECT_TRUE(Service.UnregisterImporter(ProviderId));
	EXPECT_EQ(Operation.GetSnapshot().State,
		Durin::Asset::EImportOperationState::Canceled);
	EXPECT_TRUE(ValueDestroyed->load());
	EXPECT_TRUE(JobDestroyed->load());
	EXPECT_EQ(Service.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(ProviderDestroyed->load());
}

TEST(FAssetImportCoreTests, OperationHandleDetachesFromLegacyInitiatorAndRetainsOneOutcome)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	auto& Registry = Durin::Asset::GetImportService();
	Durin::Asset::FAsyncImportPlanHandle Legacy = Registry.BeginAsyncImportOperation(
		"Tests.Operation.DetachedOwner", "Tests.Operation.Provider", "Detached operation");
	ASSERT_TRUE(Legacy);
	const Durin::Asset::FImportOperationHandle Operation = Legacy.GetOperationHandle();
	ASSERT_TRUE(Operation);
	EXPECT_EQ(Operation.GetOperationId(), Legacy.GetSerial());
	Legacy = {};

	EXPECT_TRUE(Operation.RequestCancel());
	Registry.CancelAndDrainAllAsyncImports();
	const Durin::Asset::FImportOperationSnapshot Terminal = Operation.GetSnapshot();
	EXPECT_EQ(Terminal.State, Durin::Asset::EImportOperationState::Canceled);
	EXPECT_TRUE(Terminal.IsTerminal());

	Durin::Asset::FImportOutcome Outcome;
	ASSERT_TRUE(Operation.TryGetOutcome(Outcome));
	EXPECT_EQ(Outcome.State, Durin::Asset::EImportOperationState::Canceled);
	EXPECT_TRUE(Outcome.IsTerminal());
	const Durin::Asset::FImportOutcome FirstOutcome = Outcome;
	EXPECT_FALSE(Operation.RequestCancel());
	ASSERT_TRUE(Operation.TryGetOutcome(Outcome));
	EXPECT_EQ(Outcome, FirstOutcome);
}

TEST(FAssetImportCoreTests, ExtendedOperationCarriesExecutionProgressPastPlanResult)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreExtendedOperation");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.ExtendedOperation";
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId)},
		GetImportRegistryTestGate(), Error)) << Error;

	const Durin::Asset::FAsyncImportPlanHandle Handle =
		Registry.LaunchAsyncImportPlan({
			.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
			.ProviderId = ProviderId},
			"Tests.ExtendedOperation.Owner", true);
	Durin::Asset::FImportPlanResult Plan;
	ASSERT_EQ(WaitForAsyncResult(Handle, Plan),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Plan);
	EXPECT_FALSE(Handle.GetOperationSnapshot().IsTerminal());

	const std::shared_ptr<Durin::Asset::IImportProgressReporter> Progress =
		Handle.CreateProgressReporter();
	ASSERT_TRUE(Progress);
	Progress->Report({
		.Phase = Durin::Asset::EImportPhase::CandidateBuild,
		.State = Durin::Asset::EImportProgressState::Started,
		.SourceIdentity = "root",
		.OutputIdentity = "mesh"});
	EXPECT_FALSE(Handle.GetOperationSnapshot().Progress.has_value());
	Progress->Report({
		.Phase = Durin::Asset::EImportPhase::CandidateBuild,
		.State = Durin::Asset::EImportProgressState::Succeeded,
		.SourceIdentity = "root",
		.OutputIdentity = "mesh",
		.CompletedWork = 3,
		.TotalWork = 3});
	ASSERT_TRUE(Handle.GetOperationSnapshot().Progress.has_value());
	EXPECT_FLOAT_EQ(*Handle.GetOperationSnapshot().Progress, 1.0f);
	Progress->Report({
		.Phase = Durin::Asset::EImportPhase::Publication,
		.State = Durin::Asset::EImportProgressState::Started,
		.SourceIdentity = "root",
		.OutputIdentity = "request"});
	EXPECT_EQ(Handle.GetOperationSnapshot().State,
		Durin::Asset::EImportOperationState::Finalizing);
	EXPECT_FALSE(Handle.GetOperationSnapshot().bCancelable);
	EXPECT_TRUE(Handle.CompleteOperation(
		Durin::Asset::EImportOperationState::Succeeded));
	const Durin::Asset::FImportOperationSnapshot Terminal =
		Handle.GetOperationSnapshot();
	EXPECT_TRUE(Terminal.IsTerminal());
	EXPECT_EQ(Terminal.State, Durin::Asset::EImportOperationState::Succeeded);
	EXPECT_FALSE(Handle.CompleteOperation(
		Durin::Asset::EImportOperationState::Failed, "late failure"));
	EXPECT_EQ(Handle.GetOperationSnapshot(), Terminal);

	Plan = {};
	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
}

TEST(FAssetImportCoreTests, ProviderBarrierDrainsExtendedOperationTaskScope)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreExtendedUnload");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.ExtendedUnload";
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId)},
		GetImportRegistryTestGate(), Error)) << Error;

	const Durin::Asset::FAsyncImportPlanHandle Handle =
		Registry.LaunchAsyncImportPlan({
			.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
			.ProviderId = ProviderId},
			"Tests.ExtendedUnload.Owner", true);
	Durin::Asset::FImportPlanResult Plan;
	ASSERT_EQ(WaitForAsyncResult(Handle, Plan),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Plan);
	Plan = {};
	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);

	std::mutex Mutex;
	std::condition_variable Condition;
	bool bEntered = false;
	bool bObservedCancellation = false;
	Durin::FTaskLaunchOptions Options;
	Options.Scope = Handle.GetOperationTaskScope();
	const Durin::FTaskHandle ExecutionTask = Durin::LaunchCancelableTask(
		"AssetImportCoreTests.ExtendedExecution",
		[&](const Durin::FTaskCancellationToken& Token) {
			{
				std::lock_guard Lock(Mutex);
				bEntered = true;
			}
			Condition.notify_all();
			while (!Token.IsCancellationRequested())
				std::this_thread::yield();
			{
				std::lock_guard Lock(Mutex);
				bObservedCancellation = true;
			}
			Condition.notify_all();
		}, Options);
	ASSERT_TRUE(ExecutionTask.IsValid());
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return bEntered; }));
	}

	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
	{
		std::lock_guard Lock(Mutex);
		EXPECT_TRUE(bObservedCancellation);
	}
	EXPECT_TRUE(ExecutionTask.IsComplete());
	EXPECT_EQ(Handle.GetOperationSnapshot().State,
		Durin::Asset::EImportOperationState::Canceled);
	const Durin::FTaskSchedulerDiagnostics Diagnostics =
		Durin::GetTaskSchedulerDiagnostics();
	EXPECT_EQ(Diagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(Diagnostics.NonquiescentScopeCount, 0u);
}

TEST(FAssetImportCoreTests, DetachedExecutionShellTransfersWorkerValueOnce)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	std::atomic<bool> bBuiltOnPreparationWorker = false;
	static const Durin::FTaskAttribution Attribution =
		Durin::RegisterTaskAttribution("AssetImportTests", "DetachedExecution");
	auto Handle = Durin::Asset::LaunchAsyncImportExecution({
		.Attribution = Attribution,
		.Build = [&bBuiltOnPreparationWorker](const Durin::FTaskCancellationToken&) {
			bBuiltOnPreparationWorker.store(
				Durin::Asset::IsImportWorkerPreparation(), std::memory_order_release);
			return Durin::Asset::FDetachedImportBuildResult{
				.bSucceeded = true,
				.Value = std::make_shared<uint32>(42)};
		}});
	ASSERT_TRUE(Handle);
	Durin::Asset::FDetachedImportBuildResult Result;
	EXPECT_EQ(WaitForDetachedResult(Handle, Result),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	EXPECT_TRUE(bBuiltOnPreparationWorker.load(std::memory_order_acquire));
	const auto Value = std::static_pointer_cast<uint32>(Result.Value);
	ASSERT_TRUE(Value);
	EXPECT_EQ(*Value, 42u);

	Durin::Asset::FDetachedImportBuildResult Second;
	EXPECT_EQ(Durin::Asset::PollAsyncImportExecution(Handle, Second),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	EXPECT_FALSE(Second.Value);
}

TEST(FAssetImportCoreTests, AcceptedExtendedOperationSharesProgressAndTaskScope)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	auto Operation = Durin::Asset::GetImportService().BeginAsyncImportOperation(
		"Tests.ManualOperation.Owner", "Tests.ManualOperation.Provider",
		"Manual import");
	ASSERT_TRUE(Operation);
	EXPECT_FALSE(Operation.GetOperationSnapshot().IsTerminal());
	const auto Progress = Operation.CreateProgressReporter();
	ASSERT_TRUE(Progress);
	Progress->Report({
		.Phase = Durin::Asset::EImportPhase::CandidateBuild,
		.State = Durin::Asset::EImportProgressState::Started,
		.CompletedWork = 1,
		.TotalWork = 2});
	static const Durin::FTaskAttribution Attribution =
		Durin::RegisterTaskAttribution("AssetImportTests", "ManualOperation");
	auto Execution = Durin::Asset::LaunchAsyncImportExecution({
		.OperationScope = Operation.GetOperationTaskScope(),
		.Attribution = Attribution,
		.Build = [](const Durin::FTaskCancellationToken&) {
			return Durin::Asset::FDetachedImportBuildResult{
				.bSucceeded = true,
				.Value = std::make_shared<uint32>(7)};
		}});
	Durin::Asset::FDetachedImportBuildResult Result;
	ASSERT_EQ(WaitForDetachedResult(Execution, Result),
		Durin::Asset::EAsyncImportPlanStatus::Succeeded);
	Progress->Report({
		.Phase = Durin::Asset::EImportPhase::Publication,
		.State = Durin::Asset::EImportProgressState::Started});
	EXPECT_EQ(Operation.GetOperationSnapshot().State,
		Durin::Asset::EImportOperationState::Finalizing);
	EXPECT_TRUE(Operation.CompleteOperation(
		Durin::Asset::EImportOperationState::Succeeded));
	EXPECT_TRUE(Operation.GetOperationSnapshot().IsTerminal());
}

TEST(FAssetImportCoreTests, DetachedExecutionShellCancelsAndDrainsOwnedScope)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bEntered = false;
	bool bObservedCancellation = false;
	auto Handle = Durin::Asset::LaunchAsyncImportExecution({
		.Build = [&](const Durin::FTaskCancellationToken& Token) {
			{
				std::lock_guard Lock(Mutex);
				bEntered = true;
			}
			Condition.notify_all();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
			{
				std::lock_guard Lock(Mutex);
				bObservedCancellation = true;
			}
			Condition.notify_all();
			return Durin::Asset::FDetachedImportBuildResult{
				.bCanceled = true, .Message = "canceled"};
		}});
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return bEntered; }));
	}
	Durin::Asset::CancelAndDrainAsyncImportExecution(Handle);
	{
		std::lock_guard Lock(Mutex);
		EXPECT_TRUE(bObservedCancellation);
	}
	Durin::Asset::FDetachedImportBuildResult Result;
	EXPECT_EQ(Durin::Asset::PollAsyncImportExecution(Handle, Result),
		Durin::Asset::EAsyncImportPlanStatus::Canceled);
	const Durin::FTaskSchedulerDiagnostics Diagnostics =
		Durin::GetTaskSchedulerDiagnostics();
	EXPECT_EQ(Diagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(Diagnostics.NonquiescentScopeCount, 0u);
}

TEST(FAssetImportCoreTests, AsyncProgressSnapshotOutlivesReporterAndStabilizesAtCancellation)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreAsyncProgress");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.AsyncProgress";
	const std::string OwnerId = "Tests.AsyncProgress.Owner";
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState)},
		GetImportRegistryTestGate(), Error)) << Error;

	Durin::Asset::FAsyncImportPlanHandle Handle;
	{
		FProgressRecorder InitiatingReporter;
		Handle = Registry.LaunchAsyncImportPlan({
			.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
			.ProviderId = ProviderId,
			.Progress = &InitiatingReporter}, OwnerId);
		ASSERT_TRUE(Handle);
	}
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}

	const Durin::Asset::FImportOperationSnapshot Running =
		Handle.GetOperationSnapshot();
	EXPECT_EQ(Running.OperationId, Handle.GetSerial());
	EXPECT_EQ(Running.OwnerId, OwnerId);
	EXPECT_EQ(Running.ProviderId, ProviderId);
		EXPECT_EQ(Running.State, Durin::Asset::EImportOperationState::Pending);
		EXPECT_TRUE(BlockingState->bObservedWorkerPreparation);
		EXPECT_FALSE(Durin::Asset::IsImportWorkerPreparation());
	EXPECT_TRUE(Running.bCancelable);
	EXPECT_GE(static_cast<uint8>(Running.Phase),
		static_cast<uint8>(Durin::Asset::EImportPhase::Snapshot));
	EXPECT_TRUE(Handle.SetRunningInBackground());
	EXPECT_TRUE(Handle.GetOperationSnapshot().bRunningInBackground);

	EXPECT_TRUE(Registry.CancelAsyncImport(Handle));
	const Durin::Asset::FImportOperationSnapshot Canceling =
		Handle.GetOperationSnapshot();
	EXPECT_EQ(Canceling.State, Durin::Asset::EImportOperationState::Canceling);
	EXPECT_FALSE(Canceling.bCancelable);
	EXPECT_GT(Canceling.Revision, Running.Revision);
	EXPECT_EQ(Registry.CancelAndDrainAsyncImport(Handle),
		Durin::Asset::EAsyncImportPlanStatus::Canceled);

	const Durin::Asset::FImportOperationSnapshot Terminal =
		Handle.GetOperationSnapshot();
	EXPECT_TRUE(Terminal.IsTerminal());
	EXPECT_EQ(Terminal.State, Durin::Asset::EImportOperationState::Canceled);
	EXPECT_FALSE(Terminal.bCancelable);
	EXPECT_FALSE(Handle.SetRunningInBackground(false));
	EXPECT_EQ(Handle.GetOperationSnapshot(), Terminal);
	const std::vector<Durin::Asset::FImportOperationSnapshot> History =
		Handle.GetProgressHistory();
	ASSERT_FALSE(History.empty());
	EXPECT_LE(History.size(), Durin::Asset::MaximumAsyncImportProgressHistory);
	for (size_t Index = 1; Index < History.size(); ++Index)
		EXPECT_LT(History[Index - 1].Revision, History[Index].Revision);

	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
}

TEST(FAssetImportCoreTests, ProviderBarrierCancelsWorkerAndReleasesLeaseBeforeUnload)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreAsyncUnload");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.AsyncUnload";
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState)},
		GetImportRegistryTestGate(), Error))
		<< Error;
	const auto Handle = Durin::Asset::GetImportService().LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId}, "Tests.AsyncUnload.Owner");
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}
	Durin::Asset::GetImportService().CancelAndDrainAsyncImportsForProvider(ProviderId);
	EXPECT_EQ(Handle.GetStatus(), Durin::Asset::EAsyncImportPlanStatus::Canceled);
	const Durin::FTaskSchedulerDiagnostics TaskDiagnostics =
		Durin::GetTaskSchedulerDiagnostics();
	EXPECT_EQ(TaskDiagnostics.LiveScopeCount, 1u);
	EXPECT_EQ(TaskDiagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(TaskDiagnostics.NonquiescentScopeCount, 0u);
	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
}

TEST(FAssetImportCoreTests, OwnerBarrierCancelsRequestAndReachesScopeQuiescence)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreAsyncOwnerClose");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Registry = Durin::Asset::GetImportService();
	const std::string ProviderId = "Tests.AsyncOwnerClose";
	const std::string OwnerId = "Tests.AsyncOwnerClose.Owner";
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState)},
		GetImportRegistryTestGate(), Error))
		<< Error;
	const auto Handle = Durin::Asset::GetImportService().LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId}, OwnerId);
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}

	Durin::Asset::GetImportService().CancelAndDrainAsyncImportsForOwner(OwnerId);
	EXPECT_EQ(Handle.GetStatus(), Durin::Asset::EAsyncImportPlanStatus::Canceled);
	const Durin::FTaskSchedulerDiagnostics TaskDiagnostics =
		Durin::GetTaskSchedulerDiagnostics();
	EXPECT_EQ(TaskDiagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(TaskDiagnostics.NonquiescentScopeCount, 0u);
	EXPECT_EQ(Registry.GetOutstandingImporterLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.UnregisterImporter(ProviderId));
}

TEST(FAssetImportCoreTests, RejectedSchedulerLaunchIsReportedAsNeverAccepted)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	const auto Handle = Durin::Asset::GetImportService().LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Unavailable.graph"},
		.ProviderId = "Tests.Rejected"}, "Tests.Rejected.Owner");
	ASSERT_TRUE(Handle);
	EXPECT_EQ(Handle.GetStatus(), Durin::Asset::EAsyncImportPlanStatus::Rejected);
	Durin::Asset::FImportPlanResult Result;
	EXPECT_EQ(Durin::Asset::TryTakeAsyncImportPlanResult(Handle, Result),
		Durin::Asset::EAsyncImportPlanStatus::Rejected);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::AsyncFailure);
	EXPECT_NE(Result.Message.find("never accepted"), std::string::npos);
}
