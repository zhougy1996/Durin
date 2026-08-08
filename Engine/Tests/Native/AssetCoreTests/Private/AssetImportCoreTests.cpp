#include <gtest/gtest.h>

#include "AssetImportCore.h"
#include "AsyncImport.h"
#include "AssetSystem.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"

namespace
{
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

	class FGraphProvider final : public Durin::AssetImport::IImportProvider
	{
	public:
		explicit FGraphProvider(std::string InId = "Tests.Graph", bool bInMatches = true)
			: Id(std::move(InId)), bMatches(bInMatches) {}

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> Durin::uint32 override { return 1; }

		auto CanImport(const Durin::AssetImport::FImportSourceRecognition& Source) const
			-> bool override
		{
			return bMatches && Source.Extension == ".graph"
				&& std::string_view(reinterpret_cast<const char*>(Source.Prefix.data()),
					Source.Prefix.size()).starts_with("graph");
		}

		auto CaptureSettings(
			Durin::AssetImport::FImportPayload& OutSettings,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Graph.Settings";
			OutSettings.SchemaVersion = 1;
			OutSettings.Bytes = {0x47, 0x52, 0x41, 0x50, 0x48};
			return true;
		}

		auto DiscoverDependencies(
			std::span<const Durin::AssetImport::FSourceSnapshotEntry> Sources,
			Durin::AssetImport::FDependencyRequestSink& Sink,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			for (const Durin::AssetImport::FSourceSnapshotEntry& Source : Sources)
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
			const Durin::AssetImport::FSourceSnapshot& Snapshot,
			const Durin::AssetImport::FImportPayload&,
			Durin::AssetImport::FImportPlanBuilder& Builder,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
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
					.Policy = Durin::AssetImport::EImportOutputPolicy::Create,
					.Collision = Durin::AssetImport::EImportCollisionAction::Create,
					.EstimatedCpuBytes = It->ByteCount,
					.EstimatedDiskBytes = It->ByteCount});
			}
			Builder.SetProviderData(std::make_shared<const Durin::uint32>(17));
			return true;
		}

	private:
		std::string Id;
		bool bMatches = true;
	};

	class FTaskSchedulerGuard
	{
	public:
		~FTaskSchedulerGuard()
		{
			Durin::AssetImport::CancelAndDrainAllAsyncImports();
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

	class FBlockingGraphProvider final : public Durin::AssetImport::IImportProvider
	{
	public:
		FBlockingGraphProvider(std::string InId,
			std::shared_ptr<FBlockingProviderState> InState)
			: Id(std::move(InId)), State(std::move(InState)) {}

		auto GetProviderId() const -> std::string_view override { return Id; }
		auto GetContractVersion() const -> Durin::uint32 override { return 1; }
		auto CanImport(const Durin::AssetImport::FImportSourceRecognition& Source) const
			-> bool override { return Source.Extension == ".graph"; }
		auto CaptureSettings(Durin::AssetImport::FImportPayload& OutSettings,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			OutSettings.SchemaId = "Tests.Blocking.Settings";
			OutSettings.SchemaVersion = 1;
			OutSettings.Bytes = {1};
			return true;
		}
		auto DiscoverDependencies(
			std::span<const Durin::AssetImport::FSourceSnapshotEntry>,
			Durin::AssetImport::FDependencyRequestSink&,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			return true;
		}
		auto Plan(const Durin::AssetImport::FSourceSnapshot&,
			const Durin::AssetImport::FImportPayload&,
			Durin::AssetImport::FImportPlanBuilder& Builder,
			std::vector<Durin::AssetImport::FImportDiagnostic>&) const -> bool override
		{
			{
				std::lock_guard Lock(State->Mutex);
				State->bEntered = true;
			}
			State->Condition.notify_all();
			std::unique_lock Lock(State->Mutex);
			while (!State->bRelease
				&& !Durin::AssetImport::IsImportCancellationRequested())
				State->Condition.wait_for(Lock, std::chrono::milliseconds(1));
			if (Durin::AssetImport::IsImportCancellationRequested()) return false;
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
		const Durin::AssetImport::FAsyncImportPlanHandle& Handle,
		Durin::AssetImport::FImportPlanResult& OutResult)
		-> Durin::AssetImport::EAsyncImportPlanStatus
	{
		for (Durin::uint32 Attempt = 0; Attempt < 5'000; ++Attempt)
		{
			(void)Durin::AssetImport::DrainAsyncImportCompletionMailbox();
			const auto Status = Durin::AssetImport::TryTakeAsyncImportPlanResult(
				Handle, OutResult);
			if (Status != Durin::AssetImport::EAsyncImportPlanStatus::Pending)
				return Status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Durin::AssetImport::EAsyncImportPlanStatus::Pending;
	}

	class FProgressRecorder final : public Durin::AssetImport::IImportProgressReporter
	{
	public:
		auto Report(const Durin::AssetImport::FImportProgressEvent& Event) noexcept
			-> void override
		{
			Events.push_back(Event);
		}
		std::vector<Durin::AssetImport::FImportProgressEvent> Events;
	};

	auto RegisterGraphProvider(
		Durin::AssetImport::FProviderRegistry& Registry,
		std::string_view Id = "Tests.Graph") -> void
	{
		std::string Error;
		ASSERT_TRUE(Registry.Register(
			std::make_shared<FGraphProvider>(std::string(Id)), Error)) << Error;
	}
}

TEST(FAssetImportCoreTests, CapturedBytesRemainImmutableAfterPhysicalSourceChanges)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreImmutable");
	WriteSource(Root / "Content" / "Root.graph", "graph\nembedded payload original\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);

	Durin::AssetImport::FSourceSnapshotBuilder Builder;
	std::vector<Durin::AssetImport::FImportDiagnostic> Diagnostics;
	ASSERT_TRUE(Builder.CaptureRoot({.Path = "/ImportCoreTests/Root.graph"}, Diagnostics));
	WriteSource(Root / "Content" / "Root.graph", "graph\nchanged after capture\n");
	ASSERT_TRUE(Builder.DiscoverDependencies(Registry.Find("Tests.Graph"), Diagnostics));
	const std::shared_ptr<const Durin::AssetImport::FSourceSnapshot> Snapshot =
		Builder.Freeze(Diagnostics);
	ASSERT_NE(Snapshot, nullptr);
	const Durin::AssetImport::FSourceSnapshotEntry* Captured = Snapshot->FindSource("root");
	ASSERT_NE(Captured, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Captured->GetBytes().data()),
		Captured->GetBytes().size()), "graph\nembedded payload original\n");
	const Durin::AssetImport::FSourceSnapshotEntry* Embedded = Snapshot->FindSource("payload");
	ASSERT_NE(Embedded, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Embedded->GetBytes().data()),
		Embedded->GetBytes().size()), "original");
	Durin::AssetImport::FImportPayload Settings;
	ASSERT_TRUE(Registry.Find("Tests.Graph").GetProvider()->CaptureSettings(
		Settings, Diagnostics));
	std::string SettingsError;
	ASSERT_TRUE(Settings.Finalize(SettingsError)) << SettingsError;
	const auto Plan = Durin::AssetImport::BuildImportPlan(
		Registry.Find("Tests.Graph"), Snapshot, Settings, Registry.GetRevision());
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
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);

	const Durin::AssetImport::FImportPlanResult Traversal = Durin::AssetImport::CreateImportPlan(
		{{.Path = "/ImportCoreTests/Traversal.graph"}}, Registry);
	ASSERT_FALSE(Traversal);
	EXPECT_EQ(Traversal.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::UnsafeDependency);

	const Durin::AssetImport::FImportPlanResult Missing = Durin::AssetImport::CreateImportPlan(
		{{.Path = "/ImportCoreTests/Missing.graph"}}, Registry);
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::MissingDependency);
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
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);

	const Durin::AssetImport::FImportPlanResult Result = Durin::AssetImport::CreateImportPlan(
		{{.Path = "/ImportCoreTests/Root.graph"}}, Registry);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Plan.GetSnapshot().GetSources().size(), 2u);
	EXPECT_EQ(Result.Plan.GetSnapshot().GetSources()[0].StableIdentity, "child");
	EXPECT_EQ(Result.Plan.GetSnapshot().GetSources()[1].StableIdentity, "root");
	EXPECT_TRUE(std::ranges::any_of(Result.Diagnostics,
		[](const Durin::AssetImport::FImportDiagnostic& Diagnostic) {
			return Diagnostic.Category
				== Durin::AssetImport::EImportDiagnosticCategory::MissingDependency
				&& Diagnostic.Severity
					== Durin::AssetImport::EImportDiagnosticSeverity::Warning;
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
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);

	Durin::AssetImport::FImportPlanRequest CountRequest{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	CountRequest.Limits.MaximumSourceCount = 1;
	const auto CountResult = Durin::AssetImport::CreateImportPlan(CountRequest, Registry);
	ASSERT_FALSE(CountResult);
	EXPECT_EQ(CountResult.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::AssetImport::FImportPlanRequest ByteRequest = CountRequest;
	ByteRequest.Limits.MaximumSourceCount = 8;
	ByteRequest.Limits.MaximumBytesPerSource = 4;
	const auto ByteResult = Durin::AssetImport::CreateImportPlan(ByteRequest, Registry);
	ASSERT_FALSE(ByteResult);
	EXPECT_EQ(ByteResult.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::AssetImport::FImportPlanRequest SettingsRequest = CountRequest;
	SettingsRequest.Limits.MaximumSourceCount = 8;
	SettingsRequest.Limits.MaximumSettingsBytes = 2;
	const auto SettingsResult = Durin::AssetImport::CreateImportPlan(SettingsRequest, Registry);
	ASSERT_FALSE(SettingsResult);
	EXPECT_EQ(SettingsResult.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ResourceLimitExceeded);

	Durin::AssetImport::FImportPlanRequest DepthRequest = CountRequest;
	DepthRequest.Limits.MaximumSourceCount = 8;
	DepthRequest.Limits.MaximumDependencyDepth = 0;
	const auto DepthResult = Durin::AssetImport::CreateImportPlan(DepthRequest, Registry);
	ASSERT_FALSE(DepthResult);
	EXPECT_EQ(DepthResult.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ResourceLimitExceeded);

	WriteSource(Root / "Content" / "Embedded.graph", "graph\nembedded payload bytes\n");
	Durin::AssetImport::FImportPlanRequest EmbeddedRequest{
		.RootSource = {.Path = "/ImportCoreTests/Embedded.graph"}};
	EmbeddedRequest.Limits.MaximumEmbeddedBytes = 2;
	const auto EmbeddedResult = Durin::AssetImport::CreateImportPlan(EmbeddedRequest, Registry);
	ASSERT_FALSE(EmbeddedResult);
	EXPECT_EQ(EmbeddedResult.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ResourceLimitExceeded);
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
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);
	const Durin::uint64 RegistryRevision = Durin::Asset::GetAssetRegistry().GetRevision();

	const Durin::AssetImport::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	const auto First = Durin::AssetImport::CreateImportPlan(Request, Registry);
	const auto Second = Durin::AssetImport::CreateImportPlan(Request, Registry);
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;
	EXPECT_EQ(First.Plan.GetFingerprint(), Second.Plan.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		First.Plan.GetOutputs(), Second.Plan.GetOutputs()));
	EXPECT_EQ(First.Plan.GetSnapshot().GetSources().size(), 4u);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetRevision(), RegistryRevision);
	for (const Durin::AssetImport::FImportOutputPreview& Output : First.Plan.GetOutputs())
	{
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(Output.AssetPath), nullptr);
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
	Durin::AssetImport::FProviderRegistry Registry;

	const Durin::AssetImport::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"}};
	const auto Absent = Durin::AssetImport::CreateImportPlan(Request, Registry);
	ASSERT_FALSE(Absent);
	EXPECT_EQ(Absent.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ProviderUnavailable);

	RegisterGraphProvider(Registry, "Tests.First");
	RegisterGraphProvider(Registry, "Tests.Second");
	const auto Ambiguous = Durin::AssetImport::CreateImportPlan(Request, Registry);
	ASSERT_FALSE(Ambiguous);
	EXPECT_EQ(Ambiguous.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ProviderAmbiguous);

	Durin::AssetImport::FProviderLease Lease = Registry.Find("Tests.First");
	ASSERT_TRUE(Lease);
	ASSERT_TRUE(Registry.Unregister("Tests.First"));
	EXPECT_TRUE(Lease);
	EXPECT_EQ(Lease.GetProviderId(), "Tests.First");
	EXPECT_FALSE(Registry.Find("Tests.First"));

	Durin::AssetImport::FImportPlanRequest Explicit = Request;
	Explicit.ProviderId = "Tests.First";
	const auto Unregistered = Durin::AssetImport::CreateImportPlan(Explicit, Registry);
	ASSERT_FALSE(Unregistered);
	EXPECT_EQ(Unregistered.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::ProviderUnavailable);
}

TEST(FAssetImportCoreTests, ReportsSynchronousPhaseBoundariesAndDiagnosticContext)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetImportCoreProgress");
	WriteSource(Root / "Content" / "Root.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);
	FProgressRecorder Progress;
	const auto Planned = Durin::AssetImport::CreateImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.Progress = &Progress}, Registry);
	ASSERT_TRUE(Planned) << Planned.Message;
	const std::array Expected = {
		std::pair{Durin::AssetImport::EImportPhase::Snapshot,
			Durin::AssetImport::EImportProgressState::Started},
		std::pair{Durin::AssetImport::EImportPhase::Snapshot,
			Durin::AssetImport::EImportProgressState::Succeeded},
		std::pair{Durin::AssetImport::EImportPhase::Parse,
			Durin::AssetImport::EImportProgressState::Started},
		std::pair{Durin::AssetImport::EImportPhase::Parse,
			Durin::AssetImport::EImportProgressState::Succeeded},
		std::pair{Durin::AssetImport::EImportPhase::Plan,
			Durin::AssetImport::EImportProgressState::Started},
		std::pair{Durin::AssetImport::EImportPhase::Plan,
			Durin::AssetImport::EImportProgressState::Succeeded}};
	ASSERT_EQ(Progress.Events.size(), Expected.size());
	for (size_t Index = 0; Index < Expected.size(); ++Index)
	{
		EXPECT_EQ(Progress.Events[Index].Phase, Expected[Index].first);
		EXPECT_EQ(Progress.Events[Index].State, Expected[Index].second);
		EXPECT_FALSE(Progress.Events[Index].SourceIdentity.empty());
		EXPECT_FALSE(Progress.Events[Index].OutputIdentity.empty());
	}

	FProgressRecorder FailureProgress;
	Durin::AssetImport::FProviderRegistry EmptyRegistry;
	const auto Failed = Durin::AssetImport::CreateImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.Progress = &FailureProgress}, EmptyRegistry);
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
		Durin::AssetImport::EImportProgressState::Failed);
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
	auto& Registry = Durin::AssetImport::GetProviderRegistry();
	const std::string ProviderId = "Tests.AsyncEquivalence";
	Durin::AssetImport::OpenAsyncImportProviderAdmission(ProviderId);
	std::string Error;
	ASSERT_TRUE(Registry.Register(
		std::make_shared<FGraphProvider>(ProviderId), Error)) << Error;

	Durin::AssetImport::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId};
	Durin::AssetImport::FImportPlanResult Synchronous =
		Durin::AssetImport::CreateImportPlan(Request, Registry);
	ASSERT_TRUE(Synchronous) << Synchronous.Message;
	const Durin::AssetImport::FAsyncImportPlanHandle Handle =
		Durin::AssetImport::LaunchAsyncImportPlan(
			Request, "Tests.AsyncEquivalence.Owner");
	ASSERT_TRUE(Handle);
	Durin::AssetImport::FImportPlanResult Asynchronous;
	ASSERT_EQ(WaitForAsyncResult(Handle, Asynchronous),
		Durin::AssetImport::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Asynchronous) << Asynchronous.Message;
	EXPECT_EQ(Synchronous.Plan.GetFingerprint(), Asynchronous.Plan.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		Synchronous.Plan.GetOutputs(), Asynchronous.Plan.GetOutputs()));
	EXPECT_EQ(Synchronous.Diagnostics, Asynchronous.Diagnostics);
	EXPECT_EQ(Durin::AssetImport::DrainAsyncImportCompletionMailbox(), 0u);
	Durin::AssetImport::FImportPlanResult SecondTake;
	SecondTake.Message = "unchanged";
	EXPECT_EQ(Durin::AssetImport::TryTakeAsyncImportPlanResult(Handle, SecondTake),
		Durin::AssetImport::EAsyncImportPlanStatus::Succeeded);
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
	EXPECT_EQ(Registry.GetOutstandingLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.Unregister(ProviderId));
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
	auto& Registry = Durin::AssetImport::GetProviderRegistry();
	const std::string ProviderId = "Tests.AsyncSerial";
	Durin::AssetImport::OpenAsyncImportProviderAdmission(ProviderId);
	std::string Error;
	ASSERT_TRUE(Registry.Register(
		std::make_shared<FGraphProvider>(ProviderId), Error)) << Error;
	const Durin::AssetImport::FImportPlanRequest Request{
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId};
	const auto First = Durin::AssetImport::LaunchAsyncImportPlan(
		Request, "Tests.AsyncSerial.Owner");
	const auto Second = Durin::AssetImport::LaunchAsyncImportPlan(
		Request, "Tests.AsyncSerial.Owner");
	ASSERT_LT(First.GetSerial(), Second.GetSerial());
	Durin::AssetImport::FImportPlanResult FirstResult;
	EXPECT_EQ(WaitForAsyncResult(First, FirstResult),
		Durin::AssetImport::EAsyncImportPlanStatus::Superseded);
	Durin::AssetImport::FImportPlanResult SecondResult;
	EXPECT_EQ(WaitForAsyncResult(Second, SecondResult),
		Durin::AssetImport::EAsyncImportPlanStatus::Succeeded);
	EXPECT_TRUE(SecondResult);
	SecondResult = {};
	EXPECT_EQ(Registry.GetOutstandingLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.Unregister(ProviderId));
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
	auto& Registry = Durin::AssetImport::GetProviderRegistry();
	const std::string ProviderId = "Tests.AsyncUnload";
	Durin::AssetImport::OpenAsyncImportProviderAdmission(ProviderId);
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.Register(
		std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState), Error))
		<< Error;
	const auto Handle = Durin::AssetImport::LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId}, "Tests.AsyncUnload.Owner");
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}
	Durin::AssetImport::CancelAndDrainAsyncImportsForProvider(ProviderId);
	EXPECT_EQ(Handle.GetStatus(), Durin::AssetImport::EAsyncImportPlanStatus::Canceled);
	const Durin::FTaskSchedulerDiagnostics TaskDiagnostics =
		Durin::GetTaskSchedulerDiagnostics();
	EXPECT_EQ(TaskDiagnostics.LiveScopeCount, 1u);
	EXPECT_EQ(TaskDiagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(TaskDiagnostics.NonquiescentScopeCount, 0u);
	EXPECT_EQ(Registry.GetOutstandingLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.Unregister(ProviderId));
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
	auto& Registry = Durin::AssetImport::GetProviderRegistry();
	const std::string ProviderId = "Tests.AsyncOwnerClose";
	const std::string OwnerId = "Tests.AsyncOwnerClose.Owner";
	Durin::AssetImport::OpenAsyncImportProviderAdmission(ProviderId);
	const auto BlockingState = std::make_shared<FBlockingProviderState>();
	std::string Error;
	ASSERT_TRUE(Registry.Register(
		std::make_shared<FBlockingGraphProvider>(ProviderId, BlockingState), Error))
		<< Error;
	const auto Handle = Durin::AssetImport::LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Root.graph"},
		.ProviderId = ProviderId}, OwnerId);
	ASSERT_TRUE(Handle);
	{
		std::unique_lock Lock(BlockingState->Mutex);
		ASSERT_TRUE(BlockingState->Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
	}

	Durin::AssetImport::CancelAndDrainAsyncImportsForOwner(OwnerId);
	EXPECT_EQ(Handle.GetStatus(), Durin::AssetImport::EAsyncImportPlanStatus::Canceled);
	const Durin::FTaskSchedulerDiagnostics TaskDiagnostics =
		Durin::GetTaskSchedulerDiagnostics();
	EXPECT_EQ(TaskDiagnostics.OpenScopeCount, 0u);
	EXPECT_EQ(TaskDiagnostics.NonquiescentScopeCount, 0u);
	EXPECT_EQ(Registry.GetOutstandingLeaseCount(ProviderId), 0u);
	EXPECT_TRUE(Registry.Unregister(ProviderId));
}

TEST(FAssetImportCoreTests, RejectedSchedulerLaunchIsReportedAsNeverAccepted)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	const auto Handle = Durin::AssetImport::LaunchAsyncImportPlan({
		.RootSource = {.Path = "/ImportCoreTests/Unavailable.graph"},
		.ProviderId = "Tests.Rejected"}, "Tests.Rejected.Owner");
	ASSERT_TRUE(Handle);
	EXPECT_EQ(Handle.GetStatus(), Durin::AssetImport::EAsyncImportPlanStatus::Rejected);
	Durin::AssetImport::FImportPlanResult Result;
	EXPECT_EQ(Durin::AssetImport::TryTakeAsyncImportPlanResult(Handle, Result),
		Durin::AssetImport::EAsyncImportPlanStatus::Rejected);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::AsyncFailure);
	EXPECT_NE(Result.Message.find("never accepted"), std::string::npos);
}
