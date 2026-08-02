#include <gtest/gtest.h>

#include "AssetImportCore.h"
#include "AssetSystem.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

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
			.OwnerRoot = Root,
			.ContentRoot = Root / "Content",
			.SourceAssetsRoot = Root / "SourceAssets",
			.bSourceWritable = true};
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
	WriteSource(Root / "SourceAssets" / "Root.graph", "graph\nembedded payload original\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::AssetImport::FProviderRegistry Registry;
	RegisterGraphProvider(Registry);

	Durin::AssetImport::FSourceSnapshotBuilder Builder;
	std::vector<Durin::AssetImport::FImportDiagnostic> Diagnostics;
	ASSERT_TRUE(Builder.CaptureRoot({.Path = "/ImportCoreTests/Root.graph"}, Diagnostics));
	WriteSource(Root / "SourceAssets" / "Root.graph", "graph\nchanged after capture\n");
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
	WriteSource(Root / "SourceAssets" / "Traversal.graph", "graph\ndep escape ../Outside.bin\n");
	WriteSource(Root / "SourceAssets" / "Missing.graph", "graph\ndep absent Missing.bin\n");
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
	WriteSource(Root / "SourceAssets" / "Root.graph",
		"graph\ndep child Child.graph\ndep child Child.graph\noptional absent Missing.bin\n");
	WriteSource(Root / "SourceAssets" / "Child.graph", "graph\ndep root Root.graph\n");
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
	WriteSource(Root / "SourceAssets" / "Root.graph", "graph\ndep child Child.graph\n");
	WriteSource(Root / "SourceAssets" / "Child.graph", "graph\n");
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

	WriteSource(Root / "SourceAssets" / "Embedded.graph", "graph\nembedded payload bytes\n");
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
	WriteSource(Root / "SourceAssets" / "Root.graph",
		"graph\ndep beta B.graph\ndep alpha A.graph\nembedded inline bytes\n");
	WriteSource(Root / "SourceAssets" / "A.graph", "graph\n");
	WriteSource(Root / "SourceAssets" / "B.graph", "graph\n");
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
	WriteSource(Root / "SourceAssets" / "Root.graph", "graph\n");
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
	WriteSource(Root / "SourceAssets" / "Root.graph", "graph\n");
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
