#include <gtest/gtest.h>

#include "AssetImportCore.h"
#include "AsyncImport.h"
#include "ImportService.h"
#include "AssetLoad.h"
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

	class FGraphProvider final : public Durin::Asset::Import::IImportProvider
	{
	public:
		explicit FGraphProvider(
			std::string InId = "Tests.Graph", bool bInMatches = true,
			std::shared_ptr<std::atomic_bool> InDestroyed = {})
			: Id(std::move(InId)), bMatches(bInMatches), Destroyed(std::move(InDestroyed)) {}
		~FGraphProvider() override { if (Destroyed) *Destroyed = true; }

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> Durin::uint32 override { return 1; }

		auto CanImport(const Durin::Asset::Import::FImportSourceRecognition& Source) const
			-> bool override
		{
			return bMatches && Source.Extension == ".graph"
				&& std::string_view(reinterpret_cast<const char*>(Source.Prefix.data()),
					Source.Prefix.size()).starts_with("graph");
		}

		auto CaptureSettings(
			Durin::Asset::Import::FImportPayload& OutSettings,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Graph.Settings";
			OutSettings.SchemaVersion = 1;
			OutSettings.Bytes = {0x47, 0x52, 0x41, 0x50, 0x48};
			return true;
		}

		auto DiscoverDependencies(
			std::span<const Durin::Asset::Import::FSourceSnapshotEntry> Sources,
			Durin::Asset::Import::FDependencyRequestSink& Sink,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&) const -> bool override
		{
			for (const Durin::Asset::Import::FSourceSnapshotEntry& Source : Sources)
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
						const std::span Bytes(
							reinterpret_cast<const Durin::uint8*>(Value.data()), Value.size());
						if (!Sink.AddEmbedded(Source.StableIdentity, Identity, "Embedded", Bytes))
							return false;
					}
				}
			}
			return true;
		}

		auto Plan(
			const Durin::Asset::Import::FSourceSnapshot& Snapshot,
			const Durin::Asset::Import::FImportPayload&,
			Durin::Asset::Import::FImportPlanBuilder& Builder,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&) const -> bool override
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
					.Policy = Durin::Asset::Import::EImportOutputPolicy::Create,
					.Collision = Durin::Asset::Import::EImportCollisionAction::Create,
					.EstimatedCpuBytes = It->ByteCount,
					.EstimatedDiskBytes = It->ByteCount});
			}
			Builder.SetProviderData(std::make_shared<const Durin::uint32>(17));
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
			Durin::Asset::Import::GetImportService().CancelAndDrainAllAsyncImports();
			Durin::ShutdownTaskScheduler(false);
		}
	};

	struct FBlockingProviderState
	{
		std::mutex Mutex;
		std::condition_variable Condition;
		bool bEntered = false;
		bool bRelease = false;
	};

	class FBlockingGraphProvider final : public Durin::Asset::Import::IImportProvider
	{
	public:
		FBlockingGraphProvider(std::string InId,
			std::shared_ptr<FBlockingProviderState> InState)
			: Id(std::move(InId)), State(std::move(InState)) {}

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> Durin::uint32 override { return 1; }
		auto CanImport(const Durin::Asset::Import::FImportSourceRecognition& Source) const
			-> bool override { return Source.Extension == ".graph"; }
		auto CaptureSettings(Durin::Asset::Import::FImportPayload& OutSettings,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Blocking.Settings";
			OutSettings.SchemaVersion = 1;
			OutSettings.Bytes = {1};
			return true;
		}
		auto DiscoverDependencies(
			std::span<const Durin::Asset::Import::FSourceSnapshotEntry>,
			Durin::Asset::Import::FDependencyRequestSink&,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&) const -> bool override
		{
			return true;
		}
		auto Plan(const Durin::Asset::Import::FSourceSnapshot&,
			const Durin::Asset::Import::FImportPayload&,
			Durin::Asset::Import::FImportPlanBuilder& Builder,
			std::vector<Durin::Asset::Import::FImportDiagnostic>&) const -> bool override
		{
			{
				std::lock_guard Lock(State->Mutex);
				State->bEntered = true;
			}
			State->Condition.notify_all();
			std::unique_lock Lock(State->Mutex);
			while (!State->bRelease
				&& !Durin::Asset::Import::IsImportCancellationRequested())
				State->Condition.wait_for(Lock, std::chrono::milliseconds(1));
			if (Durin::Asset::Import::IsImportCancellationRequested()) return false;
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
		const Durin::Asset::Import::FAsyncImportPlanHandle& Handle,
		Durin::Asset::Import::FImportPlanResult& OutResult)
		-> Durin::Asset::Import::EAsyncImportPlanStatus
	{
		for (Durin::uint32 Attempt = 0; Attempt < 5'000; ++Attempt)
		{
			(void)Durin::Asset::Import::DrainAsyncImportCompletionMailbox();
			const auto Status = Durin::Asset::Import::TryTakeAsyncImportPlanResult(
				Handle, OutResult);
			if (Status != Durin::Asset::Import::EAsyncImportPlanStatus::Pending)
				return Status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Durin::Asset::Import::EAsyncImportPlanStatus::Pending;
	}

	class FProgressRecorder final : public Durin::Asset::Import::IImportProgressReporter
	{
	public:
		auto Report(const Durin::Asset::Import::FImportProgressEvent& Event) noexcept
			-> void override
		{
			Events.push_back(Event);
		}
		std::vector<Durin::Asset::Import::FImportProgressEvent> Events;
	};

	auto RegisterGraphProvider(
		Durin::Asset::Import::FImportService& Registry,
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
	Durin::Asset::Import::FImportService Service;
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

TEST(FAssetImportCoreTests, ProviderOwnerRetirementRejectsLookupAndAuditsLeaseDestruction)
{
	Durin::FModuleTestOwner Context("AssetImportCoreTests.ProviderRetirement");
	auto Registration = Context.CreateOwnedCallbackRegistration(
		"AssetImportCore.ProviderRegistry");
	Durin::Asset::Import::FImportService Registry;
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
	Durin::Asset::Import::FImportService Registry;
	RegisterGraphProvider(Registry);

	Durin::Asset::Import::FSourceSnapshotBuilder Builder;
	std::vector<Durin::Asset::Import::FImportDiagnostic> Diagnostics;
	ASSERT_TRUE(Builder.CaptureRoot({.Path = "/ImportCoreTests/Root.graph"}, Diagnostics));
	WriteSource(Root / "Content" / "Root.graph", "graph\nchanged after capture\n");
	ASSERT_TRUE(Builder.DiscoverDependencies(Registry.FindImporter("Tests.Graph"), Diagnostics));
	const std::shared_ptr<const Durin::Asset::Import::FSourceSnapshot> Snapshot =
		Builder.Freeze(Diagnostics);
	ASSERT_NE(Snapshot, nullptr);
	const Durin::Asset::Import::FSourceSnapshotEntry* Captured = Snapshot->FindSource("root");
	ASSERT_NE(Captured, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Captured->GetBytes().data()),
		Captured->GetBytes().size()), "graph\nembedded payload original\n");
	const Durin::Asset::Import::FSourceSnapshotEntry* Embedded = Snapshot->FindSource("payload");
	ASSERT_NE(Embedded, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Embedded->GetBytes().data()),
		Embedded->GetBytes().size()), "original");
	Durin::Asset::Import::FImportPayload Settings;
	ASSERT_TRUE(Registry.FindImporter("Tests.Graph").GetProvider()->CaptureSettings(
		Settings, Diagnostics));
	std::string SettingsError;
	ASSERT_TRUE(Settings.Finalize(SettingsError)) << SettingsError;
	const auto Plan = Durin::Asset::Import::BuildImportPlan(
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
	Durin::Asset::Import::FImportService Registry;
	RegisterGraphProvider(Registry);

	const Durin::Asset::Import::FImportPlanResult Traversal = Registry.CreateImportPlan(
		{{.Path = "/ImportCoreTests/Traversal.graph"}});
	ASSERT_FALSE(Traversal);
	EXPECT_EQ(Traversal.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::UnsafeDependency);

	const Durin::Asset::Import::FImportPlanResult Missing = Registry.CreateImportPlan(
		{{.Path = "/ImportCoreTests/Missing.graph"}});
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::MissingDependency);
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
	Durin::Asset::Import::FImportService Registry;
	RegisterGraphProvider(Registry);

	const Durin::Asset::Import::FImportPlanResult Result = Registry.CreateImportPlan(
		{{.Path = "/ImportCoreTests/Root.graph"}});
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Plan.GetSnapshot().GetSources().size(), 2u);
	EXPECT_EQ(Result.Plan.GetSnapshot().GetSources()[0].StableIdentity, "child");
	EXPECT_EQ(Result.Plan.GetSnapshot().GetSources()[1].StableIdentity, "root");
	EXPECT_TRUE(std::ranges::any_of(Result.Diagnostics,
		[](const Durin::Asset::Import::FImportDiagnostic& Diagnostic) {
			return Diagnostic.Category
				== Durin::Asset::Import::EImportDiagnosticCategory::MissingDependency
				&& Diagnostic.Severity
					== Durin::Asset::Import::EImportDiagnosticSeverity::Warning;
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
	Durin::Asset::Import::FImportService Registry;
	RegisterGraphProvider(Registry);

	Durin::Asset::Import::FImportPlanRequest CountRequest{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	CountRequest.Limits.MaximumSourceCount = 1;
	const auto CountResult = Registry.CreateImportPlan(CountRequest);
	ASSERT_FALSE(CountResult);
	EXPECT_EQ(CountResult.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::Asset::Import::FImportPlanRequest ByteRequest = CountRequest;
	ByteRequest.Limits.MaximumSourceCount = 8;
	ByteRequest.Limits.MaximumBytesPerSource = 4;
	const auto ByteResult = Registry.CreateImportPlan(ByteRequest);
	ASSERT_FALSE(ByteResult);
	EXPECT_EQ(ByteResult.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::Asset::Import::FImportPlanRequest SettingsRequest = CountRequest;
	SettingsRequest.Limits.MaximumSourceCount = 8;
	SettingsRequest.Limits.MaximumSettingsBytes = 2;
	const auto SettingsResult = Registry.CreateImportPlan(SettingsRequest);
	ASSERT_FALSE(SettingsResult);
	EXPECT_EQ(SettingsResult.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::Asset::Import::FImportPlanRequest DepthRequest = CountRequest;
	DepthRequest.Limits.MaximumSourceCount = 8;
	DepthRequest.Limits.MaximumDependencyDepth = 0;
	const auto DepthResult = Registry.CreateImportPlan(DepthRequest);
	ASSERT_FALSE(DepthResult);
	EXPECT_EQ(DepthResult.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ResourceLimitExceeded);

	WriteSource(Root / "Content" / "Embedded.graph", "graph\nembedded payload bytes\n");
	Durin::Asset::Import::FImportPlanRequest EmbeddedRequest{
		.RootSource = {.Path = "/ImportCoreTests/Embedded.graph"}};
	EmbeddedRequest.Limits.MaximumEmbeddedBytes = 2;
	const auto EmbeddedResult = Registry.CreateImportPlan(EmbeddedRequest);
	ASSERT_FALSE(EmbeddedResult);
	EXPECT_EQ(EmbeddedResult.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ResourceLimitExceeded);
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
	Durin::Asset::Import::FImportService Registry;
	RegisterGraphProvider(Registry);
	const Durin::uint64 RegistryRevision = Durin::Asset::GetAssetCatalogRevision();

	const Durin::Asset::Import::FImportPlanRequest Request{
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
	for (const Durin::Asset::Import::FImportOutputPreview& Output : First.Plan.GetOutputs())
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
	Durin::Asset::Import::FImportService Registry;

	const Durin::Asset::Import::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	const auto Absent = Registry.CreateImportPlan(Request);
	ASSERT_FALSE(Absent);
	EXPECT_EQ(Absent.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ProviderUnavailable);

	RegisterGraphProvider(Registry, "Tests.First");
	RegisterGraphProvider(Registry, "Tests.Second");
	const auto Ambiguous = Registry.CreateImportPlan(Request);
	ASSERT_FALSE(Ambiguous);
	EXPECT_EQ(Ambiguous.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ProviderAmbiguous);

	Durin::Asset::Import::FProviderLease Lease = Registry.FindImporter("Tests.First");
	ASSERT_TRUE(Lease);
	ASSERT_TRUE(Registry.UnregisterImporter("Tests.First"));
	EXPECT_TRUE(Lease);
	EXPECT_EQ(Lease.GetProviderId(), "Tests.First");
	EXPECT_FALSE(Registry.FindImporter("Tests.First"));

	Durin::Asset::Import::FImportPlanRequest Explicit = Request;
	Explicit.ProviderId = "Tests.First";
	const auto Unregistered = Registry.CreateImportPlan(Explicit);
	ASSERT_FALSE(Unregistered);
	EXPECT_EQ(Unregistered.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::ProviderUnavailable);
}

TEST(FAssetImportCoreTests, ReportsSynchronousPhaseBoundariesAndDiagnosticContext)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreProgress");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::Import::FImportService Registry;
	RegisterGraphProvider(Registry);
	FProgressRecorder Progress;
	const auto Planned = Registry.CreateImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.Progress = &Progress});
	ASSERT_TRUE(Planned) << Planned.Message;
	const std::array Expected = {
		std::pair{Durin::Asset::Import::EImportPhase::Snapshot,
			Durin::Asset::Import::EImportProgressState::Started},
		std::pair{Durin::Asset::Import::EImportPhase::Snapshot,
			Durin::Asset::Import::EImportProgressState::Succeeded},
		std::pair{Durin::Asset::Import::EImportPhase::Parse,
			Durin::Asset::Import::EImportProgressState::Started},
		std::pair{Durin::Asset::Import::EImportPhase::Parse,
			Durin::Asset::Import::EImportProgressState::Succeeded},
		std::pair{Durin::Asset::Import::EImportPhase::Plan,
			Durin::Asset::Import::EImportProgressState::Started},
		std::pair{Durin::Asset::Import::EImportPhase::Plan,
			Durin::Asset::Import::EImportProgressState::Succeeded}};
	ASSERT_EQ(Progress.Events.size(), Expected.size());
	for (size_t Index = 0; Index < Expected.size(); ++Index)
	{
		EXPECT_EQ(Progress.Events[Index].Phase, Expected[Index].first);
		EXPECT_EQ(Progress.Events[Index].State, Expected[Index].second);
		EXPECT_FALSE(Progress.Events[Index].SourceIdentity.empty());
		EXPECT_FALSE(Progress.Events[Index].OutputIdentity.empty());
	}

	FProgressRecorder FailureProgress;
	Durin::Asset::Import::FImportService EmptyRegistry;
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
		Durin::Asset::Import::EImportProgressState::Failed);
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
	auto& Registry = Durin::Asset::Import::GetImportService();
	const std::string ProviderId = "Tests.AsyncEquivalence";
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId)},
		GetImportRegistryTestGate(), Error)) << Error;

	Durin::Asset::Import::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId};
	Durin::Asset::Import::FImportPlanResult Synchronous =
		Registry.CreateImportPlan(Request);
	ASSERT_TRUE(Synchronous) << Synchronous.Message;
	const Durin::Asset::Import::FAsyncImportPlanHandle Handle =
		Durin::Asset::Import::GetImportService().LaunchAsyncImportPlan(
			Request, "Tests.AsyncEquivalence.Owner");
	ASSERT_TRUE(Handle);
	Durin::Asset::Import::FImportPlanResult Asynchronous;
	ASSERT_EQ(WaitForAsyncResult(Handle, Asynchronous),
		Durin::Asset::Import::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Asynchronous) << Asynchronous.Message;
	EXPECT_EQ(Synchronous.Plan.GetFingerprint(), Asynchronous.Plan.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		Synchronous.Plan.GetOutputs(), Asynchronous.Plan.GetOutputs()));
	EXPECT_EQ(Synchronous.Diagnostics, Asynchronous.Diagnostics);
	EXPECT_EQ(Durin::Asset::Import::DrainAsyncImportCompletionMailbox(), 0u);
	Durin::Asset::Import::FImportPlanResult SecondTake;
	SecondTake.Message = "unchanged";
	EXPECT_EQ(Durin::Asset::Import::TryTakeAsyncImportPlanResult(Handle, SecondTake),
		Durin::Asset::Import::EAsyncImportPlanStatus::Succeeded);
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
	auto& Registry = Durin::Asset::Import::GetImportService();
	const std::string ProviderId = "Tests.AsyncSerial";
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FGraphProvider>(ProviderId)},
		GetImportRegistryTestGate(), Error)) << Error;
	const Durin::Asset::Import::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId};
	const auto First = Durin::Asset::Import::GetImportService().LaunchAsyncImportPlan(
		Request, "Tests.AsyncSerial.Owner");
	const auto Second = Durin::Asset::Import::GetImportService().LaunchAsyncImportPlan(
		Request, "Tests.AsyncSerial.Owner");
	ASSERT_LT(First.GetSerial(), Second.GetSerial());
	Durin::Asset::Import::FImportPlanResult FirstResult;
	EXPECT_EQ(WaitForAsyncResult(First, FirstResult),
		Durin::Asset::Import::EAsyncImportPlanStatus::Superseded);
	Durin::Asset::Import::FImportPlanResult SecondResult;
	EXPECT_EQ(WaitForAsyncResult(Second, SecondResult),
		Durin::Asset::Import::EAsyncImportPlanStatus::Succeeded);
	EXPECT_TRUE(SecondResult);
	SecondResult = {};
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
	auto& Registry = Durin::Asset::Import::GetImportService();
	const std::string ProviderId = "Tests.AsyncUnload";
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState)},
		GetImportRegistryTestGate(), Error))
		<< Error;
	const auto Handle = Durin::Asset::Import::GetImportService().LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId}, "Tests.AsyncUnload.Owner");
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}
	Durin::Asset::Import::GetImportService().CancelAndDrainAsyncImportsForProvider(ProviderId);
	EXPECT_EQ(Handle.GetStatus(), Durin::Asset::Import::EAsyncImportPlanStatus::Canceled);
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
	auto& Registry = Durin::Asset::Import::GetImportService();
	const std::string ProviderId = "Tests.AsyncOwnerClose";
	const std::string OwnerId = "Tests.AsyncOwnerClose.Owner";
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.RegisterImporter({
		.Provider = std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState)},
		GetImportRegistryTestGate(), Error))
		<< Error;
	const auto Handle = Durin::Asset::Import::GetImportService().LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId}, OwnerId);
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}

	Durin::Asset::Import::GetImportService().CancelAndDrainAsyncImportsForOwner(OwnerId);
	EXPECT_EQ(Handle.GetStatus(), Durin::Asset::Import::EAsyncImportPlanStatus::Canceled);
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
	const auto Handle = Durin::Asset::Import::GetImportService().LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Unavailable.graph"},
		.ProviderId = "Tests.Rejected"}, "Tests.Rejected.Owner");
	ASSERT_TRUE(Handle);
	EXPECT_EQ(Handle.GetStatus(), Durin::Asset::Import::EAsyncImportPlanStatus::Rejected);
	Durin::Asset::Import::FImportPlanResult Result;
	EXPECT_EQ(Durin::Asset::Import::TryTakeAsyncImportPlanResult(Handle, Result),
		Durin::Asset::Import::EAsyncImportPlanStatus::Rejected);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::AsyncFailure);
	EXPECT_NE(Result.Message.find("never accepted"), std::string::npos);
}
