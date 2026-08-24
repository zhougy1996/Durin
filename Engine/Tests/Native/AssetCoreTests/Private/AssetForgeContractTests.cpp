#include "AssetTools.h"
#include "AssetForge/ImportService.h"
#include "AssetForge/Persistence/ImportRecord.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

namespace
{
	auto Bytes(std::string_view Text) -> std::vector<std::byte>
	{
		const auto Raw = std::as_bytes(std::span(Text));
		return {Raw.begin(), Raw.end()};
	}

	auto Payload(std::string_view Schema, std::string_view Text = {})
		-> Durin::AssetForge::FSchemaPayload
	{
		return {
			.SchemaId = std::string(Schema),
			.SchemaVersion = 1,
			.Bytes = Bytes(Text)};
	}

	auto AssetPath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		std::string Error;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result, &Error)) << Error;
		return Result;
	}

	auto MakeMount(const std::filesystem::path& Root)
		-> Durin::PathUtilities::FMountPoint
	{
		return {
			.VirtualRoot = "/AssetForgeTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Root,
			.bAutoScan = false,
			.bAuthoringWritable = true};
	}

	auto WriteSource(const std::filesystem::path& Path, std::string_view Text) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		ASSERT_TRUE(Stream.good());
	}

	auto BuildSourceGraph(bool bReverse)
		-> Durin::AssetForge::FSourceGraph
	{
		Durin::AssetForge::FSourceGraphBuilder Builder;
		Durin::AssetForge::FSourceNode Image{
			.StableIdentity = "source.image",
			.NodeKind = "Durin.Image",
			.Payload = Payload("Durin.Image.Rgba8", "pixels"),
			.SourceIdentities = {"root"}};
		Durin::AssetForge::FSourceNode Material{
			.StableIdentity = "source.material",
			.NodeKind = "Durin.Material",
			.Payload = Payload("Durin.Material.Pbr", "material"),
			.SourceIdentities = {"root"},
			.Dependencies = {"source.image"}};
		EXPECT_TRUE(Builder.AddNode(bReverse ? std::move(Material) : std::move(Image)));
		EXPECT_TRUE(Builder.AddNode(bReverse ? std::move(Image) : std::move(Material)));
		Durin::AssetForge::FSourceGraph Graph;
		std::vector<Durin::AssetForge::FImportDiagnostic> Diagnostics;
		EXPECT_TRUE(Builder.Finalize(Graph, Diagnostics));
		EXPECT_TRUE(Diagnostics.empty());
		return Graph;
	}

	struct FTypedPayload
	{
		static constexpr std::string_view SchemaId = "Tests.TypedPayload";
		static constexpr uint32 SchemaVersion = 2;
		std::string Value;

		static auto DecodeSchemaPayload(
			std::span<const std::byte> InBytes,
			FTypedPayload& OutValue,
			std::string&) -> bool
		{
			OutValue.Value.assign(
				reinterpret_cast<const char*>(InBytes.data()), InBytes.size());
			return true;
		}
	};

	auto GetAssetForgeTestGate() -> Durin::FModuleOwnedCallbackGate
	{
		static Durin::FModuleTestOwner Context("AssetForgeContractTests.Registry");
		static auto Registration = Context.CreateOwnedCallbackRegistration(
			"AssetForgeContractTests.Registry");
		return Registration.GetGate();
	}

	class FTestTranslator final : public Durin::AssetForge::ISourceTranslator
	{
	public:
		explicit FTestTranslator(
			std::shared_ptr<std::atomic_bool> InDestroyed = {},
			std::shared_ptr<std::atomic_uint32_t> InTranslateCount = {})
			: Destroyed(std::move(InDestroyed)),
			TranslateCount(std::move(InTranslateCount)) {}
		~FTestTranslator() override
		{
			if (Destroyed) Destroyed->store(true, std::memory_order_release);
		}

		auto Recognize(const Durin::AssetForge::FImportSourceRecognition&) const
			-> bool override { return true; }
		auto DiscoverDependencies(
			std::span<const Durin::AssetForge::FSourceSnapshotEntry>,
			Durin::AssetForge::FDependencyRequestSink&,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> bool override { return true; }
		auto Translate(
			const Durin::AssetForge::FSourceSnapshot&,
			const Durin::AssetForge::FSchemaPayload&,
			Durin::AssetForge::FSourceGraphBuilder& Builder,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> bool override
		{
			if (TranslateCount) TranslateCount->fetch_add(1, std::memory_order_relaxed);
			return Builder.AddNode({
				.StableIdentity = "source.image",
				.NodeKind = "Tests.Source",
				.Payload = Payload("Tests.Source", "source"),
				.SourceIdentities = {"root"}});
		}

	private:
		std::shared_ptr<std::atomic_bool> Destroyed;
		std::shared_ptr<std::atomic_uint32_t> TranslateCount;
	};

	class FTestPlanningPass final : public Durin::AssetForge::IPlanningPass
	{
	public:
		explicit FTestPlanningPass(Durin::FAssetPath InDestination, bool bInSucceed = true,
			std::shared_ptr<std::function<void()>> InCallback = {})
			: Destination(std::move(InDestination)), bSucceed(bInSucceed),
			Callback(std::move(InCallback)) {}

		auto Execute(
			const Durin::AssetForge::FSourceGraph&,
			const Durin::AssetForge::FBuildGraph*,
			const Durin::AssetForge::FSchemaPayload&,
			Durin::AssetForge::FBuildGraphBuilder& Builder,
			std::vector<Durin::AssetForge::FImportDiagnostic>& OutDiagnostics) const
			-> bool override
		{
			if (Callback && *Callback) (*Callback)();
			if (!bSucceed)
			{
				OutDiagnostics.push_back({
					.Category = Durin::AssetForge::EImportDiagnosticCategory::ProviderFailure,
					.Identity = "Tests.PipelineFailure",
					.Phase = "PlanningPass",
					.Message = "Synthetic pipeline failure."});
				return false;
			}
			return Builder.AddNode({
				.StableIdentity = "output.texture",
				.BuilderId = "Tests.TextureAssetBuilder",
				.BuilderContractVersion = 1,
				.OutputClassName = "DTexture2D",
				.Destination = Destination,
				.Settings = Payload("Tests.TextureAssetBuilder.Settings"),
				.SourceNodeReferences = {"source.image"}});
		}

	private:
		Durin::FAssetPath Destination;
		bool bSucceed = true;
		std::shared_ptr<std::function<void()>> Callback;
	};

	class FTestAssetBuilder final : public Durin::AssetForge::IAssetBuilder
	{
	public:
		auto BuildDetachedProduct(
			const Durin::AssetForge::FBuildNode&,
			const Durin::AssetForge::FSourceGraph&,
			Durin::AssetForge::IImportProgressReporter*,
			const std::function<bool()>&,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> std::unique_ptr<Durin::AssetForge::IBuildProduct> override
		{
			return {};
		}

		auto MaterializeCandidate(
			const Durin::AssetForge::FBuildNode&,
			std::unique_ptr<Durin::AssetForge::IBuildProduct>,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> std::unique_ptr<Durin::AssetForge::ISingleAssetCandidate> override
		{
			return {};
		}
	};

	class FJobPlanningPass final : public Durin::AssetForge::IPlanningPass
	{
	public:
		FJobPlanningPass(Durin::FAssetPath InFirst, Durin::FAssetPath InSecond,
			std::string InClassName)
			: First(std::move(InFirst)), Second(std::move(InSecond)),
			ClassName(std::move(InClassName)) {}

		auto Execute(
			const Durin::AssetForge::FSourceGraph&,
			const Durin::AssetForge::FBuildGraph*,
			const Durin::AssetForge::FSchemaPayload&,
			Durin::AssetForge::FBuildGraphBuilder& Builder,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> bool override
		{
			return Builder.AddNode({
				.StableIdentity = "output.first",
				.BuilderId = "Tests.AssetForge.JobAssetBuilder",
				.BuilderContractVersion = 1,
				.OutputClassName = ClassName,
				.Destination = First,
				.Settings = Payload("Tests.JobAssetBuilder.Settings"),
				.SourceNodeReferences = {"source.image"}})
				&& Builder.AddNode({
					.StableIdentity = "output.second",
					.BuilderId = "Tests.AssetForge.JobAssetBuilder",
					.BuilderContractVersion = 1,
					.OutputClassName = ClassName,
					.Destination = Second,
					.Settings = Payload("Tests.JobAssetBuilder.Settings"),
					.SourceNodeReferences = {"source.image"},
					.BuildDependencies = {"output.first"}});
		}

	private:
		Durin::FAssetPath First;
		Durin::FAssetPath Second;
		std::string ClassName;
	};

	class FBlockingTranslator final : public Durin::AssetForge::ISourceTranslator
	{
	public:
		explicit FBlockingTranslator(std::shared_ptr<std::atomic_bool> InEntered)
			: Entered(std::move(InEntered)) {}
		auto Recognize(const Durin::AssetForge::FImportSourceRecognition&) const
			-> bool override { return true; }
		auto DiscoverDependencies(
			std::span<const Durin::AssetForge::FSourceSnapshotEntry>,
			Durin::AssetForge::FDependencyRequestSink&,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> bool override { return true; }
		auto Translate(
			const Durin::AssetForge::FSourceSnapshot&,
			const Durin::AssetForge::FSchemaPayload&,
			Durin::AssetForge::FSourceGraphBuilder&,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> bool override
		{
			Entered->store(true, std::memory_order_release);
			while (!Durin::AssetForge::IsImportCancellationRequested())
				std::this_thread::yield();
			return false;
		}

	private:
		std::shared_ptr<std::atomic_bool> Entered;
	};

	class FJobProduct final : public Durin::AssetForge::IBuildProduct
	{
	public:
		auto CloneDetachedProduct() const
			-> std::unique_ptr<Durin::AssetForge::IBuildProduct> override
		{
			return std::make_unique<FJobProduct>();
		}
	};

	class FJobCandidate final : public Durin::AssetForge::ISingleAssetCandidate
	{
	public:
		explicit FJobCandidate(Durin::AssetForge::DImportRecord& InRecord)
			: Record(&InRecord) {}

		auto GetAsset() const -> Durin::DObject* override { return Record; }
		auto GetPackage() const -> Durin::DPackage* override
		{
			return Record ? Record->GetPackage() : nullptr;
		}
		auto IsNewAsset() const -> bool override { return true; }
		auto GetAuthoredFingerprint() const -> std::string override
		{
			return Record ? Record->GetFingerprint() : std::string{};
		}
		auto Validate(std::vector<Durin::AssetForge::FImportDiagnostic>& OutDiagnostics) const
			-> bool override
		{
			std::string Error;
			if (Record && Record->Validate(Error)) return true;
			OutDiagnostics.push_back({
				.Category = Durin::AssetForge::EImportDiagnosticCategory::ValidationFailure,
				.Identity = "Tests.JobCandidateInvalid",
				.Phase = "Validation",
				.Message = std::move(Error)});
			return false;
		}
		auto Abandon() noexcept -> void override
		{
			if (Durin::DPackage* Detached = DetachPackageForAbandon())
				(void)Durin::Asset::UnloadPackage(Detached,
					Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		}
		auto DetachPackageForAbandon() noexcept -> Durin::DPackage* override
		{
			Durin::DPackage* Detached = Record ? Record->GetPackage() : nullptr;
			Record = nullptr;
			return Detached;
		}

	private:
		Durin::AssetForge::DImportRecord* Record = nullptr;
	};

	class FJobAssetBuilder final : public Durin::AssetForge::IAssetBuilder
	{
	public:
		explicit FJobAssetBuilder(std::shared_ptr<std::atomic_uint32_t> InBuildCount)
			: BuildCount(std::move(InBuildCount)) {}
		auto BuildDetachedProduct(
			const Durin::AssetForge::FBuildNode&,
			const Durin::AssetForge::FSourceGraph&,
			Durin::AssetForge::IImportProgressReporter*,
			const std::function<bool()>& IsCancellationRequested,
			std::vector<Durin::AssetForge::FImportDiagnostic>&) const
			-> std::unique_ptr<Durin::AssetForge::IBuildProduct> override
		{
			BuildCount->fetch_add(1, std::memory_order_relaxed);
			return IsCancellationRequested() ? nullptr : std::make_unique<FJobProduct>();
		}

		auto MaterializeCandidate(
			const Durin::AssetForge::FBuildNode& Node,
			std::unique_ptr<Durin::AssetForge::IBuildProduct>,
			std::vector<Durin::AssetForge::FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<Durin::AssetForge::ISingleAssetCandidate> override
		{
			Durin::AssetForge::DImportRecord* Record = nullptr;
			const Durin::Asset::FAssetResult Created =
				Durin::AssetForge::CreateImportRecordAsset(Node.Destination, Record);
			if (!Created || !Record)
			{
				OutDiagnostics.push_back({
					.Category = Durin::AssetForge::EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Tests.JobCandidateCreateFailed",
					.Phase = "CandidateBuild",
					.Message = Created.Message});
				return {};
			}
			Durin::AssetForge::FImportRecordPayload Settings;
			Durin::AssetForge::FImportRecordPayload StatePayload;
			std::string Error;
			if (!Durin::AssetForge::MakeImportRecordPayload(
				"Tests.Settings", 1, {}, 64, Settings, Error)
				|| !Durin::AssetForge::MakeImportRecordPayload(
					"Tests.State", 1, {}, 64, StatePayload, Error))
			{
				(void)Durin::Asset::UnloadPackage(Record->GetPackage(),
					Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
				return {};
			}
			Durin::AssetForge::FImportRecordState State{
				.ProviderId = "Tests.AssetForge.Job",
				.ProviderContractVersion = 1,
				.Settings = std::move(Settings),
				.ProviderState = std::move(StatePayload),
				.Sources = {{
					.StableIdentity = "root", .Role = "Root",
					.SourcePath = {.Path = "/AssetForgeTests/source.graph"},
					.ContentHashLow = 1, .ContentHashHigh = 2, .ByteCount = 1}},
				.Outputs = {{
					.StableIdentity = Node.StableIdentity,
					.Role = "Synthetic",
					.AssetPath = Node.Destination,
					.AssetClassName = Node.OutputClassName,
					.Policy = Durin::AssetForge::EImportRecordOutputPolicy::Managed,
					.AuthoredFingerprint = "synthetic"}}};
			if (!Record->SetState(std::move(State), Error))
			{
				(void)Durin::Asset::UnloadPackage(Record->GetPackage(),
					Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
				OutDiagnostics.push_back({
					.Category = Durin::AssetForge::EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Tests.JobCandidateStateFailed",
					.Phase = "CandidateBuild",
					.Message = std::move(Error)});
				return {};
			}
			return std::make_unique<FJobCandidate>(*Record);
		}
	private:
		std::shared_ptr<std::atomic_uint32_t> BuildCount;
	};

	class FTaskSchedulerGuard
	{
	public:
		~FTaskSchedulerGuard() { Durin::ShutdownTaskScheduler(false); }
	};
}

TEST(FAssetForgeContractTests, SourceGraphCanonicalizesOrderAndFingerprint)
{
	const auto Forward = BuildSourceGraph(false);
	const auto Reverse = BuildSourceGraph(true);
	ASSERT_TRUE(std::ranges::equal(Forward.GetNodes(), Reverse.GetNodes()));
	EXPECT_EQ(Forward.GetFingerprint(), Reverse.GetFingerprint());
	ASSERT_EQ(Forward.GetNodes().size(), 2u);
	EXPECT_EQ(Forward.GetNodes()[0].StableIdentity, "source.image");
	EXPECT_EQ(Forward.GetNodes()[1].StableIdentity, "source.material");
	EXPECT_NE(Forward.FindNode("source.material"), nullptr);
}

TEST(FAssetForgeContractTests, SourceGraphRejectsMissingNodesAndCycles)
{
	Durin::AssetForge::FSourceGraphBuilder MissingBuilder;
	ASSERT_TRUE(MissingBuilder.AddNode({
		.StableIdentity = "source.mesh",
		.NodeKind = "Durin.Mesh",
		.Payload = Payload("Durin.Mesh", "mesh"),
		.Dependencies = {"source.missing"}}));
	Durin::AssetForge::FSourceGraph Graph;
	std::vector<Durin::AssetForge::FImportDiagnostic> Diagnostics;
	EXPECT_FALSE(MissingBuilder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Identity, "Durin.AssetForge.Diagnostic.MissingDependency");

	Durin::AssetForge::FSourceGraphBuilder CycleBuilder;
	ASSERT_TRUE(CycleBuilder.AddNode({
		.StableIdentity = "a", .NodeKind = "Tests.Node",
		.Payload = Payload("Tests.Node", "a"), .Dependencies = {"b"}}));
	ASSERT_TRUE(CycleBuilder.AddNode({
		.StableIdentity = "b", .NodeKind = "Tests.Node",
		.Payload = Payload("Tests.Node", "b"), .Dependencies = {"a"}}));
	Diagnostics.clear();
	EXPECT_FALSE(CycleBuilder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Category,
		Durin::AssetForge::EImportDiagnosticCategory::DependencyCycle);
}

TEST(FAssetForgeContractTests, GraphBuildersEnforceResourceLimits)
{
	Durin::AssetForge::FGraphLimits Limits;
	Limits.MaximumNodes = 1;
	Limits.MaximumPayloadBytes = 2;
	Durin::AssetForge::FSourceGraphBuilder Builder(Limits);
	EXPECT_TRUE(Builder.AddNode({
		.StableIdentity = "a", .NodeKind = "Tests.Node",
		.Payload = Payload("Tests.Node", "abc")}));
	EXPECT_FALSE(Builder.AddNode({
		.StableIdentity = "b", .NodeKind = "Tests.Node",
		.Payload = Payload("Tests.Node", "b")}));
	Durin::AssetForge::FSourceGraph Graph;
	std::vector<Durin::AssetForge::FImportDiagnostic> Diagnostics;
	EXPECT_FALSE(Builder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Category,
		Durin::AssetForge::EImportDiagnosticCategory::ResourceLimitExceeded);
}

TEST(FAssetForgeContractTests, AssetBuilderGraphValidatesReferencesAndCanonicalizesDependencies)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgeFactoryGraph");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const Durin::AssetForge::FSourceGraph Source = BuildSourceGraph(false);
	Durin::AssetForge::FBuildGraphBuilder Builder(Source);
	ASSERT_TRUE(Builder.AddNode({
		.StableIdentity = "output.material",
		.BuilderId = "Durin.MaterialAssetBuilder",
		.BuilderContractVersion = 1,
		.OutputClassName = "DMaterial",
		.Destination = AssetPath("/AssetForgeTests/Materials/TestMaterial"),
		.Settings = Payload("Durin.MaterialAssetBuilder.Settings"),
		.SourceNodeReferences = {"source.material", "source.image"}}));
	ASSERT_TRUE(Builder.AddNode({
		.StableIdentity = "output.mesh",
		.BuilderId = "Durin.StaticMeshAssetBuilder",
		.BuilderContractVersion = 1,
		.OutputClassName = "DStaticMesh",
		.Destination = AssetPath("/AssetForgeTests/Meshes/TestMesh"),
		.Settings = Payload("Durin.StaticMeshAssetBuilder.Settings"),
		.SourceNodeReferences = {"source.material"},
		.BuildDependencies = {"output.material"}}));
	Durin::AssetForge::FBuildGraph Graph;
	std::vector<Durin::AssetForge::FImportDiagnostic> Diagnostics;
	ASSERT_TRUE(Builder.Finalize(Graph, Diagnostics));
	EXPECT_TRUE(Diagnostics.empty());
	ASSERT_EQ(Graph.GetNodes().size(), 2u);
	EXPECT_EQ(Graph.GetNodes()[0].StableIdentity, "output.material");
	EXPECT_EQ(Graph.GetNodes()[0].SourceNodeReferences,
		(std::vector<std::string>{"source.image", "source.material"}));
	EXPECT_FALSE(Graph.GetFingerprint().IsZero());
}

TEST(FAssetForgeContractTests, BuildGraphRejectsMissingSourceReference)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgeFactoryMissingReference");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const Durin::AssetForge::FSourceGraph Source = BuildSourceGraph(false);
	Durin::AssetForge::FBuildGraphBuilder Builder(Source);
	ASSERT_TRUE(Builder.AddNode({
		.StableIdentity = "output.texture",
		.BuilderId = "Durin.TextureAssetBuilder",
		.BuilderContractVersion = 1,
		.OutputClassName = "DTexture2D",
		.Destination = AssetPath("/AssetForgeTests/Textures/Test"),
		.Settings = Payload("Durin.TextureAssetBuilder.Settings"),
		.SourceNodeReferences = {"source.unknown"}}));
	Durin::AssetForge::FBuildGraph Graph;
	std::vector<Durin::AssetForge::FImportDiagnostic> Diagnostics;
	EXPECT_FALSE(Builder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Identity, "Durin.AssetForge.Diagnostic.MissingSourceReference");
}

TEST(FAssetForgeContractTests, TypedPayloadAccessRejectsSchemaAndVersionMismatch)
{
	FTypedPayload Value;
	std::string Error;
	auto WrongSchema = Payload("Tests.OtherPayload", "value");
	WrongSchema.SchemaVersion = FTypedPayload::SchemaVersion;
	ASSERT_TRUE(WrongSchema.Finalize(Error));
	EXPECT_FALSE(Durin::AssetForge::DecodeSchemaPayload(WrongSchema, Value, Error));
	EXPECT_NE(Error.find("Durin.AssetForge.Diagnostic.SchemaMismatch"), std::string::npos);

	auto WrongVersion = Payload(FTypedPayload::SchemaId, "value");
	ASSERT_TRUE(WrongVersion.Finalize(Error));
	EXPECT_FALSE(Durin::AssetForge::DecodeSchemaPayload(WrongVersion, Value, Error));
	EXPECT_NE(Error.find("SchemaVersionMismatch"), std::string::npos);

	WrongVersion.SchemaVersion = FTypedPayload::SchemaVersion;
	ASSERT_TRUE(WrongVersion.Finalize(Error));
	EXPECT_TRUE(Durin::AssetForge::DecodeSchemaPayload(WrongVersion, Value, Error)) << Error;
	EXPECT_EQ(Value.Value, "value");
}

TEST(FAssetForgeContractTests, ProvenanceRequiresExactFrameworkIdentity)
{
	Durin::AssetForge::FImportProvenance Provenance;
	std::string Error;
	EXPECT_FALSE(Provenance.Validate(Error));
	EXPECT_NE(Error.find("ImportProvenanceIncomplete"), std::string::npos);

	Provenance.Translator.Id = "Tests.Translator";
	Provenance.Translator.ContractVersion = 1;
	Provenance.Sources.push_back({.StableIdentity = "root", .Role = "Root"});
	Provenance.SourceGraphFingerprint = Durin::FXxHash128::HashBuffer("translated");
	Provenance.BuildGraphFingerprint = Durin::FXxHash128::HashBuffer("factory");
	Provenance.PlanningPassStack.push_back({
		.PlanningPassId = "Tests.DefaultPlanningPass", .ContractVersion = 1});
	EXPECT_TRUE(Provenance.Validate(Error)) << Error;
	Provenance.PlanningPassStack.back().ContractVersion = 0;
	EXPECT_FALSE(Provenance.Validate(Error));
	EXPECT_NE(Error.find("Durin.AssetForge.Diagnostic.PlanningPassStackInvalid"), std::string::npos);
}

TEST(FAssetForgeContractTests, RegistrySelectsPriorityAndReportsAmbiguity)
{
	auto& Service = Durin::AssetForge::GetImportService();
	std::string Error;
	auto Low = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.Low", .ContractVersion = 1},
			.Extensions = {".graph"},
			.Priority = 1},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Low) << Error;
	auto High = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.High", .ContractVersion = 1},
			.Extensions = {".graph"},
			.Priority = 2},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(High) << Error;

	const Durin::AssetForge::FImportSourceRecognition Source{.Extension = ".GRAPH"};
	auto Selected = Service.SelectSourceTranslator(Source);
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected.Lease.GetId(), "Tests.AssetForge.High");
	Selected = {};
	ASSERT_TRUE(High.Reset());

	auto Equal = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.Equal", .ContractVersion = 1},
			.Extensions = {".graph"},
			.Priority = 1},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Equal) << Error;
	Selected = Service.SelectSourceTranslator(Source);
	EXPECT_FALSE(Selected);
	ASSERT_FALSE(Selected.Diagnostics.empty());
	EXPECT_EQ(Selected.Diagnostics.back().Category,
		Durin::AssetForge::EImportDiagnosticCategory::ProviderAmbiguous);
	Selected = {};
	EXPECT_TRUE(Equal.Reset());
	EXPECT_TRUE(Low.Reset());
}

TEST(FAssetForgeContractTests, ExactRegistrationRetiresBeforeLeaseDestruction)
{
	auto& Service = Durin::AssetForge::GetImportService();
	const auto Destroyed = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	auto Registration = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.Lifetime", .ContractVersion = 1},
			.Extensions = {".lifetime"}},
		.Implementation = std::make_shared<FTestTranslator>(Destroyed)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;

	auto Lease = Service.FindComponent(
		Durin::AssetForge::EComponentRole::Translator,
		"Tests.AssetForge.Lifetime", 1);
	ASSERT_TRUE(Lease);
	EXPECT_TRUE(Registration.Reset());
	EXPECT_FALSE(Service.FindComponent(
		Durin::AssetForge::EComponentRole::Translator,
		"Tests.AssetForge.Lifetime", 1));
	EXPECT_FALSE(Destroyed->load(std::memory_order_acquire));
	Lease = {};
	EXPECT_TRUE(Destroyed->load(std::memory_order_acquire));
}

TEST(FAssetForgeContractTests, ConcurrentLookupStopsAtExactRetirement)
{
	auto& Service = Durin::AssetForge::GetImportService();
	const auto Destroyed = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	auto Registration = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.Concurrent", .ContractVersion = 1},
			.Extensions = {".concurrent"}},
		.Implementation = std::make_shared<FTestTranslator>(Destroyed)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	std::atomic_bool Stop = false;
	std::atomic_uint64_t Lookups = 0;
	std::jthread Reader([&] {
		while (!Stop.load(std::memory_order_acquire))
		{
			auto Lease = Service.FindComponent(
				Durin::AssetForge::EComponentRole::Translator,
				"Tests.AssetForge.Concurrent", 1);
			if (Lease) Lookups.fetch_add(1, std::memory_order_release);
		}
	});
	while (Lookups.load(std::memory_order_acquire) == 0) std::this_thread::yield();
	EXPECT_TRUE(Registration.Reset());
	EXPECT_FALSE(Service.FindComponent(
		Durin::AssetForge::EComponentRole::Translator,
		"Tests.AssetForge.Concurrent", 1));
	Stop.store(true, std::memory_order_release);
	Reader.join();
	EXPECT_TRUE(Destroyed->load(std::memory_order_acquire));
}

TEST(FAssetForgeContractTests, PersistedSelectionRequiresExactVersion)
{
	auto& Service = Durin::AssetForge::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.Versioned", .ContractVersion = 3},
			.Extensions = {".versioned"}},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	const Durin::AssetForge::FImportSourceRecognition Source{.Extension = ".versioned"};
	EXPECT_TRUE(Service.SelectSourceTranslator(Source, "Tests.AssetForge.Versioned", 3));
	auto Mismatch = Service.SelectSourceTranslator(Source, "Tests.AssetForge.Versioned", 2);
	EXPECT_FALSE(Mismatch);
	ASSERT_FALSE(Mismatch.Diagnostics.empty());
	EXPECT_EQ(Mismatch.Diagnostics.back().Identity,
		"Durin.AssetForge.Diagnostic.PersistedTranslatorUnavailable");
	Mismatch = {};
	EXPECT_TRUE(Registration.Reset());
}

TEST(FAssetForgeContractTests, AssetBuilderRegistrationSelectsByOutputClass)
{
	auto& Service = Durin::AssetForge::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterAssetBuilderScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.TextureAssetBuilder", .ContractVersion = 4},
			.OutputClassName = "DTexture2D",
			.Priority = 5},
		.Implementation = std::make_shared<FTestAssetBuilder>()},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	auto Selected = Service.SelectAssetBuilder("DTexture2D");
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected.Lease.GetId(), "Tests.AssetForge.TextureAssetBuilder");
	EXPECT_EQ(Selected.Lease.GetContractVersion(), 4u);
	EXPECT_EQ(Selected.Lease.GetOutputClassName(), "DTexture2D");
	EXPECT_EQ(Selected.Lease.GetThreadCapability(),
		Durin::AssetForge::EThreadCapability::WorkerSafe);
	EXPECT_NE(Selected.Lease.GetAssetBuilder(), nullptr);
	auto Incompatible = Service.SelectAssetBuilder(
		"DMaterial", "Tests.AssetForge.TextureAssetBuilder", 4);
	EXPECT_FALSE(Incompatible);
	ASSERT_FALSE(Incompatible.Diagnostics.empty());
	EXPECT_EQ(Incompatible.Diagnostics.back().Identity,
		"Durin.AssetForge.Diagnostic.PersistedBuilderUnavailable");
	const auto Metadata = Service.EnumerateComponents(
		Durin::AssetForge::EComponentRole::AssetBuilder);
	ASSERT_EQ(Metadata.size(), 1u);
	EXPECT_EQ(Metadata.front().Id, "Tests.AssetForge.TextureAssetBuilder");
	EXPECT_TRUE(Metadata.front().Settings.SchemaId.empty());
	Selected = {};
	EXPECT_TRUE(Registration.Reset());
}

TEST(FAssetForgeContractTests, PlanningPassStackBuildsDeterministicBuildGraph)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgePlanningPassStack");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::AssetForge::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterPlanningPassScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.AssetForge.TexturePlanningPass", .ContractVersion = 1}},
		.Implementation = std::make_shared<FTestPlanningPass>(
			AssetPath("/AssetForgeTests/Textures/Test"))},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	const auto Source = BuildSourceGraph(false);
	const std::vector Stack = {Durin::AssetForge::FPlanningPassStackEntry{
		.PlanningPassId = "Tests.AssetForge.TexturePlanningPass", .ContractVersion = 1}};
	const auto First = Durin::AssetForge::ExecutePlanningPassStack(Source, Stack);
	const auto Second = Durin::AssetForge::ExecutePlanningPassStack(Source, Stack);
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	EXPECT_EQ(First.Graph.GetFingerprint(), Second.Graph.GetFingerprint());
	ASSERT_EQ(First.Graph.GetNodes().size(), 1u);
	EXPECT_EQ(First.Graph.GetNodes().front().StableIdentity, "output.texture");
	EXPECT_TRUE(Registration.Reset());
}

TEST(FAssetForgeContractTests, PlanningPassFailureDoesNotPublishPartialGraph)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgePipelineFailure");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::AssetForge::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterPlanningPassScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.AssetForge.FailingPlanningPass", .ContractVersion = 1}},
		.Implementation = std::make_shared<FTestPlanningPass>(
			AssetPath("/AssetForgeTests/Textures/Failed"), false)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	const auto Source = BuildSourceGraph(false);
	const std::vector Stack = {Durin::AssetForge::FPlanningPassStackEntry{
		.PlanningPassId = "Tests.AssetForge.FailingPlanningPass", .ContractVersion = 1}};
	const auto Result = Durin::AssetForge::ExecutePlanningPassStack(Source, Stack);
	EXPECT_FALSE(Result);
	EXPECT_TRUE(Result.Graph.GetNodes().empty());
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Identity, "Tests.PipelineFailure");
	EXPECT_TRUE(Registration.Reset());
}

TEST(FAssetForgeContractTests, PlanningPassExecutionRejectsRegistryRevisionChange)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgePipelineRevision");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::AssetForge::GetImportService();
	const auto Callback = std::make_shared<std::function<void()>>();
	std::string Error;
	auto Registration = Service.RegisterPlanningPassScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.AssetForge.RevisionPlanningPass", .ContractVersion = 1}},
		.Implementation = std::make_shared<FTestPlanningPass>(
			AssetPath("/AssetForgeTests/Textures/Stale"), true, Callback)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	*Callback = [&] { EXPECT_TRUE(Registration.Reset()); };
	const auto Source = BuildSourceGraph(false);
	const std::vector Stack = {Durin::AssetForge::FPlanningPassStackEntry{
		.PlanningPassId = "Tests.AssetForge.RevisionPlanningPass", .ContractVersion = 1}};
	const auto Result = Durin::AssetForge::ExecutePlanningPassStack(Source, Stack);
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Identity, "Durin.AssetForge.Diagnostic.RegistryChanged");
}

TEST(FAssetForgeContractTests, ProvenanceSerializationRoundTripsCanonicalValues)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("ImportProvenance");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::AssetForge::FImportProvenance Provenance;
	std::string Error;
	Provenance.Translator = {
		.Id = "Tests.Translator",
		.ContractVersion = 2,
		.Settings = Payload("Tests.Translator.Settings", "translator")};
	ASSERT_TRUE(Provenance.Translator.Settings.Finalize(Error));
	auto PlanningPassSettings = Payload("Tests.PlanningPass.Settings", "pipeline");
	ASSERT_TRUE(PlanningPassSettings.Finalize(Error));
	Provenance.PlanningPassStack.push_back({
		.PlanningPassId = "Tests.PlanningPass", .ContractVersion = 3,
		.Settings = std::move(PlanningPassSettings)});
	Provenance.Sources.push_back({
		.StableIdentity = "root", .Role = "Root",
		.SourcePath = {.Path = "/AssetForgeTests/source.graph"},
		.ContentHash = Durin::FXxHash128::HashBuffer("source"), .ByteCount = 6});
	Provenance.OutputMappings.push_back({
		.SourceNodeIdentity = "source.image",
		.OutputIdentity = "output.texture",
		.AssetPath = AssetPath("/AssetForgeTests/Textures/Test")});
	Provenance.SourceGraphFingerprint = Durin::FXxHash128::HashBuffer("translated");
	Provenance.BuildGraphFingerprint = Durin::FXxHash128::HashBuffer("factory");
	Provenance.AuthoredOutputFingerprint = "authored";
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::AssetForge::SerializeImportProvenance(
		Provenance, Bytes, Error)) << Error;
	Durin::AssetForge::FImportProvenance RoundTrip;
	ASSERT_TRUE(Durin::AssetForge::DeserializeImportProvenance(
		Bytes, RoundTrip, Error)) << Error;
	EXPECT_EQ(RoundTrip, Provenance);
}

TEST(FAssetForgeContractTests, UnifiedJobPublishesSyntheticGraphInlineAndScheduled)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgeUnifiedJob");
	WriteSource(Root / "source.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::AssetForge::GetImportService();
	const auto TranslateCount = std::make_shared<std::atomic_uint32_t>(0);
	const auto BuildCount = std::make_shared<std::atomic_uint32_t>(0);
	const std::string ClassName = Durin::AssetForge::DImportRecord::StaticClass()
		->GetQualifiedName().ToString();
	std::string Error;
	auto Translator = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.JobTranslator", .ContractVersion = 1},
			.Extensions = {".graph"}},
		.Implementation = std::make_shared<FTestTranslator>(nullptr, TranslateCount)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Translator) << Error;
	auto AssetBuilder = Service.RegisterAssetBuilderScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.JobAssetBuilder", .ContractVersion = 1},
			.OutputClassName = ClassName},
		.Implementation = std::make_shared<FJobAssetBuilder>(BuildCount)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(AssetBuilder) << Error;
	const Durin::FAssetPath InlineFirst = AssetPath("/AssetForgeTests/Inline/First");
	const Durin::FAssetPath InlineSecond = AssetPath("/AssetForgeTests/Inline/Second");
	const Durin::FAssetPath ScheduledFirst = AssetPath("/AssetForgeTests/Scheduled/First");
	const Durin::FAssetPath ScheduledSecond = AssetPath("/AssetForgeTests/Scheduled/Second");
	const Durin::FAssetPath FailedFirst = AssetPath("/AssetForgeTests/Failed/First");
	const Durin::FAssetPath FailedSecond = AssetPath("/AssetForgeTests/Failed/Second");
	auto InlinePlanningPass = Service.RegisterPlanningPassScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.AssetForge.InlinePlanningPass", .ContractVersion = 1}},
		.Implementation = std::make_shared<FJobPlanningPass>(
			InlineFirst, InlineSecond, ClassName)}, GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(InlinePlanningPass) << Error;
	auto ScheduledPlanningPass = Service.RegisterPlanningPassScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.AssetForge.ScheduledPlanningPass", .ContractVersion = 1}},
		.Implementation = std::make_shared<FJobPlanningPass>(
			ScheduledFirst, ScheduledSecond, ClassName)}, GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(ScheduledPlanningPass) << Error;
	auto FailingPlanningPass = Service.RegisterPlanningPassScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.AssetForge.FailingPublicationPlanningPass", .ContractVersion = 1}},
		.Implementation = std::make_shared<FJobPlanningPass>(
			FailedFirst, FailedSecond, ClassName)}, GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(FailingPlanningPass) << Error;

	Durin::AssetForge::FImportRequest InlineRequest{
		.RootSource = {.Path = "/AssetForgeTests/source.graph"},
		.TranslatorId = "Tests.AssetForge.JobTranslator",
		.PlanningPassStack = {{
			.PlanningPassId = "Tests.AssetForge.InlinePlanningPass", .ContractVersion = 1}},
		.Destination = InlineSecond,
		.Owner = {
			.OwnerId = "Tests.AssetForge.InlineJob",
			.ConflictIdentities = {InlineFirst.ToString(), InlineSecond.ToString()}}};
	auto PreviewRequest = InlineRequest;
	PreviewRequest.Mode = Durin::AssetForge::EImportMode::Preview;
	PreviewRequest.Lifetime = Durin::AssetForge::EImportOperationLifetime::EphemeralPreview;
	const Durin::AssetForge::FImportResult Preview =
		Service.RunImportInline(PreviewRequest, "AssetForge preview test");
	ASSERT_EQ(Preview.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded);
	EXPECT_EQ(TranslateCount->load(std::memory_order_relaxed), 1u);
	EXPECT_EQ(BuildCount->load(std::memory_order_relaxed), 2u);
	const Durin::AssetForge::FImportResult Inline =
		Service.RunImportInline(InlineRequest, "Inline interchange test");
	ASSERT_EQ(Inline.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Inline.Outcome.Diagnostic;
	EXPECT_EQ(TranslateCount->load(std::memory_order_relaxed), 1u);
	EXPECT_EQ(BuildCount->load(std::memory_order_relaxed), 2u);
	EXPECT_EQ(Inline.Outcome.PublishedAssetIdentities.size(), 2u);
	EXPECT_EQ(Inline.Provenance.OutputMappings.size(), 2u);
	EXPECT_TRUE(Durin::Asset::FindAssetExact(InlineFirst));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(InlineSecond));

	Durin::AssetForge::FImportRequest ScheduledRequest{
		.RootSource = {.Path = "/AssetForgeTests/source.graph"},
		.TranslatorId = "Tests.AssetForge.JobTranslator",
		.PlanningPassStack = {{
			.PlanningPassId = "Tests.AssetForge.ScheduledPlanningPass", .ContractVersion = 1}},
		.Destination = ScheduledSecond,
		.Owner = {
			.OwnerId = "Tests.AssetForge.ScheduledJob",
			.ConflictIdentities = {ScheduledFirst.ToString(), ScheduledSecond.ToString()}}};
	const auto Handle = Service.SubmitImport(
		ScheduledRequest, "Scheduled interchange test");
	ASSERT_TRUE(Handle);
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Handle.GetOperationHandle().GetSnapshot().IsTerminal()
		&& std::chrono::steady_clock::now() < Deadline)
	{
		Service.PumpImportOperations();
		std::this_thread::yield();
	}
	ASSERT_TRUE(Handle.GetOperationHandle().GetSnapshot().IsTerminal());
	Durin::AssetForge::FImportResult Scheduled;
	ASSERT_TRUE(Handle.TryGetResult(Scheduled));
	ASSERT_EQ(Scheduled.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Scheduled.Outcome.Diagnostic;
	EXPECT_EQ(Scheduled.Outcome.PublishedAssetIdentities.size(), 2u);
	EXPECT_EQ(Scheduled.Provenance.SourceGraphFingerprint,
		Inline.Provenance.SourceGraphFingerprint);
	EXPECT_TRUE(Durin::Asset::FindAssetExact(ScheduledFirst));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(ScheduledSecond));

	Durin::AssetForge::FImportRequest FailedRequest{
		.RootSource = {.Path = "/AssetForgeTests/source.graph"},
		.TranslatorId = "Tests.AssetForge.JobTranslator",
		.PlanningPassStack = {{
			.PlanningPassId = "Tests.AssetForge.FailingPublicationPlanningPass",
			.ContractVersion = 1}},
		.Destination = FailedSecond,
		.Owner = {
			.OwnerId = "Tests.AssetForge.FailedJob",
			.ConflictIdentities = {FailedFirst.ToString(), FailedSecond.ToString()}},
		.SaveOptions = {.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
			return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
		}}};
	const auto Failed = Service.RunImportInline(
		FailedRequest, "Failed interchange publication test");
	EXPECT_EQ(Failed.Outcome.State, Durin::AssetForge::EImportOperationState::Failed);
	EXPECT_FALSE(Durin::Asset::FindAssetExact(FailedFirst));
	EXPECT_FALSE(Durin::Asset::FindAssetExact(FailedSecond));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(FailedFirst), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(FailedSecond), nullptr);

	EXPECT_TRUE(Durin::Asset::UnloadPackage(InlineFirst));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(InlineSecond));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(ScheduledFirst));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(ScheduledSecond));
	EXPECT_TRUE(FailingPlanningPass.Reset());
	EXPECT_TRUE(ScheduledPlanningPass.Reset());
	EXPECT_TRUE(InlinePlanningPass.Reset());
	EXPECT_TRUE(AssetBuilder.Reset());
	EXPECT_TRUE(Translator.Reset());
}

TEST(FAssetForgeContractTests, UnifiedJobCancellationProducesOneTerminalOutcome)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("AssetForgeJobCancellation");
	WriteSource(Root / "source.block", "block\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::AssetForge::GetImportService();
	const auto Entered = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	auto Translator = Service.RegisterSourceTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.AssetForge.BlockingTranslator", .ContractVersion = 1},
			.Extensions = {".block"}},
		.Implementation = std::make_shared<FBlockingTranslator>(Entered)},
		GetAssetForgeTestGate(), Error);
	ASSERT_TRUE(Translator) << Error;
	const auto Handle = Service.SubmitImport({
		.RootSource = {.Path = "/AssetForgeTests/source.block"},
		.TranslatorId = "Tests.AssetForge.BlockingTranslator",
		.PlanningPassStack = {{.PlanningPassId = "Tests.Unreached", .ContractVersion = 1}},
		.Owner = {.OwnerId = "Tests.AssetForge.CancelJob"}},
		"Cancelable interchange test");
	ASSERT_TRUE(Handle);
	Service.PumpImportOperations();
	const auto EnterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Entered->load(std::memory_order_acquire)
		&& std::chrono::steady_clock::now() < EnterDeadline) std::this_thread::yield();
	ASSERT_TRUE(Entered->load(std::memory_order_acquire));
	EXPECT_TRUE(Service.CancelImportOperation(Handle.GetOperationHandle()));
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Handle.GetOperationHandle().GetSnapshot().IsTerminal()
		&& std::chrono::steady_clock::now() < Deadline)
	{
		Service.PumpImportOperations();
		std::this_thread::yield();
	}
	ASSERT_TRUE(Handle.GetOperationHandle().GetSnapshot().IsTerminal());
	Durin::AssetForge::FImportResult Result;
	ASSERT_TRUE(Handle.TryGetResult(Result));
	EXPECT_EQ(Result.Outcome.State, Durin::AssetForge::EImportOperationState::Canceled);
	Durin::AssetForge::FImportOutcome Second;
	ASSERT_TRUE(Handle.GetOperationHandle().TryGetOutcome(Second));
	EXPECT_EQ(Second, Result.Outcome);
	EXPECT_TRUE(Translator.Reset());
}

static_assert(std::is_move_constructible_v<Durin::AssetForge::FSourceGraph>);
static_assert(std::is_move_constructible_v<Durin::AssetForge::FBuildGraph>);
