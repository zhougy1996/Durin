#include "Interchange.h"
#include "AssetTools.h"
#include "ImportService.h"
#include "ImportRecord.h"
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
		-> Durin::Asset::FInterchangePayload
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
			.VirtualRoot = "/InterchangeTests/",
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

	auto BuildTranslatedGraph(bool bReverse)
		-> Durin::Asset::FTranslatedAssetGraph
	{
		Durin::Asset::FTranslatedAssetGraphBuilder Builder;
		Durin::Asset::FTranslatedAssetNode Image{
			.StableIdentity = "source.image",
			.NodeKind = "Durin.Image",
			.Payload = Payload("Durin.Image.Rgba8", "pixels"),
			.SourceIdentities = {"root"}};
		Durin::Asset::FTranslatedAssetNode Material{
			.StableIdentity = "source.material",
			.NodeKind = "Durin.Material",
			.Payload = Payload("Durin.Material.Pbr", "material"),
			.SourceIdentities = {"root"},
			.Dependencies = {"source.image"}};
		EXPECT_TRUE(Builder.AddNode(bReverse ? std::move(Material) : std::move(Image)));
		EXPECT_TRUE(Builder.AddNode(bReverse ? std::move(Image) : std::move(Material)));
		Durin::Asset::FTranslatedAssetGraph Graph;
		std::vector<Durin::Asset::FImportDiagnostic> Diagnostics;
		EXPECT_TRUE(Builder.Finalize(Graph, Diagnostics));
		EXPECT_TRUE(Diagnostics.empty());
		return Graph;
	}

	struct FTypedPayload
	{
		static constexpr std::string_view InterchangeSchemaId = "Tests.TypedPayload";
		static constexpr uint32 InterchangeSchemaVersion = 2;
		std::string Value;

		static auto DecodeInterchangePayload(
			std::span<const std::byte> InBytes,
			FTypedPayload& OutValue,
			std::string&) -> bool
		{
			OutValue.Value.assign(
				reinterpret_cast<const char*>(InBytes.data()), InBytes.size());
			return true;
		}
	};

	auto GetInterchangeTestGate() -> Durin::FModuleOwnedCallbackGate
	{
		static Durin::FModuleTestOwner Context("InterchangeContractTests.Registry");
		static auto Registration = Context.CreateOwnedCallbackRegistration(
			"InterchangeContractTests.Registry");
		return Registration.GetGate();
	}

	class FTestTranslator final : public Durin::Asset::IInterchangeTranslator
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

		auto Recognize(const Durin::Asset::FImportSourceRecognition&) const
			-> bool override { return true; }
		auto DiscoverDependencies(
			std::span<const Durin::Asset::FSourceSnapshotEntry>,
			Durin::Asset::FDependencyRequestSink&,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> bool override { return true; }
		auto Translate(
			const Durin::Asset::FSourceSnapshot&,
			const Durin::Asset::FInterchangePayload&,
			Durin::Asset::FTranslatedAssetGraphBuilder& Builder,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
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

	class FTestPipeline final : public Durin::Asset::IInterchangePipeline
	{
	public:
		explicit FTestPipeline(Durin::FAssetPath InDestination, bool bInSucceed = true,
			std::shared_ptr<std::function<void()>> InCallback = {})
			: Destination(std::move(InDestination)), bSucceed(bInSucceed),
			Callback(std::move(InCallback)) {}

		auto Execute(
			const Durin::Asset::FTranslatedAssetGraph&,
			const Durin::Asset::FImportFactoryGraph*,
			const Durin::Asset::FInterchangePayload&,
			Durin::Asset::FImportFactoryGraphBuilder& Builder,
			std::vector<Durin::Asset::FImportDiagnostic>& OutDiagnostics) const
			-> bool override
		{
			if (Callback && *Callback) (*Callback)();
			if (!bSucceed)
			{
				OutDiagnostics.push_back({
					.Category = Durin::Asset::EImportDiagnosticCategory::ProviderFailure,
					.Identity = "Tests.PipelineFailure",
					.Phase = "Pipeline",
					.Message = "Synthetic pipeline failure."});
				return false;
			}
			return Builder.AddNode({
				.StableIdentity = "output.texture",
				.FactoryId = "Tests.TextureFactory",
				.FactoryContractVersion = 1,
				.OutputClassName = "DTexture2D",
				.Destination = Destination,
				.Settings = Payload("Tests.TextureFactory.Settings"),
				.TranslatedNodeReferences = {"source.image"}});
		}

	private:
		Durin::FAssetPath Destination;
		bool bSucceed = true;
		std::shared_ptr<std::function<void()>> Callback;
	};

	class FTestFactory final : public Durin::Asset::IInterchangeFactory
	{
	public:
		auto BuildDetachedProduct(
			const Durin::Asset::FImportFactoryNode&,
			const Durin::Asset::FTranslatedAssetGraph&,
			Durin::Asset::IImportProgressReporter*,
			const std::function<bool()>&,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> std::unique_ptr<Durin::Asset::IInterchangeFactoryProduct> override
		{
			return {};
		}

		auto MaterializeCandidate(
			const Durin::Asset::FImportFactoryNode&,
			std::unique_ptr<Durin::Asset::IInterchangeFactoryProduct>,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> std::unique_ptr<Durin::Asset::ISingleAssetCandidate> override
		{
			return {};
		}
	};

	class FJobPipeline final : public Durin::Asset::IInterchangePipeline
	{
	public:
		FJobPipeline(Durin::FAssetPath InFirst, Durin::FAssetPath InSecond,
			std::string InClassName)
			: First(std::move(InFirst)), Second(std::move(InSecond)),
			ClassName(std::move(InClassName)) {}

		auto Execute(
			const Durin::Asset::FTranslatedAssetGraph&,
			const Durin::Asset::FImportFactoryGraph*,
			const Durin::Asset::FInterchangePayload&,
			Durin::Asset::FImportFactoryGraphBuilder& Builder,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> bool override
		{
			return Builder.AddNode({
				.StableIdentity = "output.first",
				.FactoryId = "Tests.Interchange.JobFactory",
				.FactoryContractVersion = 1,
				.OutputClassName = ClassName,
				.Destination = First,
				.Settings = Payload("Tests.JobFactory.Settings"),
				.TranslatedNodeReferences = {"source.image"}})
				&& Builder.AddNode({
					.StableIdentity = "output.second",
					.FactoryId = "Tests.Interchange.JobFactory",
					.FactoryContractVersion = 1,
					.OutputClassName = ClassName,
					.Destination = Second,
					.Settings = Payload("Tests.JobFactory.Settings"),
					.TranslatedNodeReferences = {"source.image"},
					.FactoryDependencies = {"output.first"}});
		}

	private:
		Durin::FAssetPath First;
		Durin::FAssetPath Second;
		std::string ClassName;
	};

	class FBlockingTranslator final : public Durin::Asset::IInterchangeTranslator
	{
	public:
		explicit FBlockingTranslator(std::shared_ptr<std::atomic_bool> InEntered)
			: Entered(std::move(InEntered)) {}
		auto Recognize(const Durin::Asset::FImportSourceRecognition&) const
			-> bool override { return true; }
		auto DiscoverDependencies(
			std::span<const Durin::Asset::FSourceSnapshotEntry>,
			Durin::Asset::FDependencyRequestSink&,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> bool override { return true; }
		auto Translate(
			const Durin::Asset::FSourceSnapshot&,
			const Durin::Asset::FInterchangePayload&,
			Durin::Asset::FTranslatedAssetGraphBuilder&,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> bool override
		{
			Entered->store(true, std::memory_order_release);
			while (!Durin::Asset::IsImportCancellationRequested())
				std::this_thread::yield();
			return false;
		}

	private:
		std::shared_ptr<std::atomic_bool> Entered;
	};

	class FJobProduct final : public Durin::Asset::IInterchangeFactoryProduct
	{
	public:
		auto CloneDetachedProduct() const
			-> std::unique_ptr<Durin::Asset::IInterchangeFactoryProduct> override
		{
			return std::make_unique<FJobProduct>();
		}
	};

	class FJobCandidate final : public Durin::Asset::ISingleAssetCandidate
	{
	public:
		explicit FJobCandidate(Durin::Asset::DImportRecord& InRecord)
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
		auto Validate(std::vector<Durin::Asset::FImportDiagnostic>& OutDiagnostics) const
			-> bool override
		{
			std::string Error;
			if (Record && Record->Validate(Error)) return true;
			OutDiagnostics.push_back({
				.Category = Durin::Asset::EImportDiagnosticCategory::ValidationFailure,
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
		Durin::Asset::DImportRecord* Record = nullptr;
	};

	class FJobFactory final : public Durin::Asset::IInterchangeFactory
	{
	public:
		explicit FJobFactory(std::shared_ptr<std::atomic_uint32_t> InBuildCount)
			: BuildCount(std::move(InBuildCount)) {}
		auto BuildDetachedProduct(
			const Durin::Asset::FImportFactoryNode&,
			const Durin::Asset::FTranslatedAssetGraph&,
			Durin::Asset::IImportProgressReporter*,
			const std::function<bool()>& IsCancellationRequested,
			std::vector<Durin::Asset::FImportDiagnostic>&) const
			-> std::unique_ptr<Durin::Asset::IInterchangeFactoryProduct> override
		{
			BuildCount->fetch_add(1, std::memory_order_relaxed);
			return IsCancellationRequested() ? nullptr : std::make_unique<FJobProduct>();
		}

		auto MaterializeCandidate(
			const Durin::Asset::FImportFactoryNode& Node,
			std::unique_ptr<Durin::Asset::IInterchangeFactoryProduct>,
			std::vector<Durin::Asset::FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<Durin::Asset::ISingleAssetCandidate> override
		{
			Durin::Asset::DImportRecord* Record = nullptr;
			const Durin::Asset::FAssetResult Created =
				Durin::Asset::CreateImportRecordAsset(Node.Destination, Record);
			if (!Created || !Record)
			{
				OutDiagnostics.push_back({
					.Category = Durin::Asset::EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Tests.JobCandidateCreateFailed",
					.Phase = "CandidateBuild",
					.Message = Created.Message});
				return {};
			}
			Durin::Asset::FImportRecordPayload Settings;
			Durin::Asset::FImportRecordPayload StatePayload;
			std::string Error;
			if (!Durin::Asset::MakeImportRecordPayload(
				"Tests.Settings", 1, {}, 64, Settings, Error)
				|| !Durin::Asset::MakeImportRecordPayload(
					"Tests.State", 1, {}, 64, StatePayload, Error))
			{
				(void)Durin::Asset::UnloadPackage(Record->GetPackage(),
					Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
				return {};
			}
			Durin::Asset::FImportRecordState State{
				.ProviderId = "Tests.Interchange.Job",
				.ProviderContractVersion = 1,
				.Settings = std::move(Settings),
				.ProviderState = std::move(StatePayload),
				.Sources = {{
					.StableIdentity = "root", .Role = "Root",
					.SourcePath = {.Path = "/InterchangeTests/source.graph"},
					.ContentHashLow = 1, .ContentHashHigh = 2, .ByteCount = 1}},
				.Outputs = {{
					.StableIdentity = Node.StableIdentity,
					.Role = "Synthetic",
					.AssetPath = Node.Destination,
					.AssetClassName = Node.OutputClassName,
					.Policy = Durin::Asset::EImportRecordOutputPolicy::Managed,
					.AuthoredFingerprint = "synthetic"}}};
			if (!Record->SetState(std::move(State), Error))
			{
				(void)Durin::Asset::UnloadPackage(Record->GetPackage(),
					Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
				OutDiagnostics.push_back({
					.Category = Durin::Asset::EImportDiagnosticCategory::CandidateFailure,
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

TEST(FInterchangeContractTests, TranslatedGraphCanonicalizesOrderAndFingerprint)
{
	const auto Forward = BuildTranslatedGraph(false);
	const auto Reverse = BuildTranslatedGraph(true);
	ASSERT_TRUE(std::ranges::equal(Forward.GetNodes(), Reverse.GetNodes()));
	EXPECT_EQ(Forward.GetFingerprint(), Reverse.GetFingerprint());
	ASSERT_EQ(Forward.GetNodes().size(), 2u);
	EXPECT_EQ(Forward.GetNodes()[0].StableIdentity, "source.image");
	EXPECT_EQ(Forward.GetNodes()[1].StableIdentity, "source.material");
	EXPECT_NE(Forward.FindNode("source.material"), nullptr);
}

TEST(FInterchangeContractTests, TranslatedGraphRejectsMissingNodesAndCycles)
{
	Durin::Asset::FTranslatedAssetGraphBuilder MissingBuilder;
	ASSERT_TRUE(MissingBuilder.AddNode({
		.StableIdentity = "source.mesh",
		.NodeKind = "Durin.Mesh",
		.Payload = Payload("Durin.Mesh", "mesh"),
		.Dependencies = {"source.missing"}}));
	Durin::Asset::FTranslatedAssetGraph Graph;
	std::vector<Durin::Asset::FImportDiagnostic> Diagnostics;
	EXPECT_FALSE(MissingBuilder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Identity, "InterchangeMissingDependency");

	Durin::Asset::FTranslatedAssetGraphBuilder CycleBuilder;
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
		Durin::Asset::EImportDiagnosticCategory::DependencyCycle);
}

TEST(FInterchangeContractTests, GraphBuildersEnforceResourceLimits)
{
	Durin::Asset::FInterchangeGraphLimits Limits;
	Limits.MaximumNodes = 1;
	Limits.MaximumPayloadBytes = 2;
	Durin::Asset::FTranslatedAssetGraphBuilder Builder(Limits);
	EXPECT_TRUE(Builder.AddNode({
		.StableIdentity = "a", .NodeKind = "Tests.Node",
		.Payload = Payload("Tests.Node", "abc")}));
	EXPECT_FALSE(Builder.AddNode({
		.StableIdentity = "b", .NodeKind = "Tests.Node",
		.Payload = Payload("Tests.Node", "b")}));
	Durin::Asset::FTranslatedAssetGraph Graph;
	std::vector<Durin::Asset::FImportDiagnostic> Diagnostics;
	EXPECT_FALSE(Builder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ResourceLimitExceeded);
}

TEST(FInterchangeContractTests, FactoryGraphValidatesReferencesAndCanonicalizesDependencies)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangeFactoryGraph");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const Durin::Asset::FTranslatedAssetGraph Translated = BuildTranslatedGraph(false);
	Durin::Asset::FImportFactoryGraphBuilder Builder(Translated);
	ASSERT_TRUE(Builder.AddNode({
		.StableIdentity = "output.material",
		.FactoryId = "Durin.MaterialFactory",
		.FactoryContractVersion = 1,
		.OutputClassName = "DMaterial",
		.Destination = AssetPath("/InterchangeTests/Materials/TestMaterial"),
		.Settings = Payload("Durin.MaterialFactory.Settings"),
		.TranslatedNodeReferences = {"source.material", "source.image"}}));
	ASSERT_TRUE(Builder.AddNode({
		.StableIdentity = "output.mesh",
		.FactoryId = "Durin.StaticMeshFactory",
		.FactoryContractVersion = 1,
		.OutputClassName = "DStaticMesh",
		.Destination = AssetPath("/InterchangeTests/Meshes/TestMesh"),
		.Settings = Payload("Durin.StaticMeshFactory.Settings"),
		.TranslatedNodeReferences = {"source.material"},
		.FactoryDependencies = {"output.material"}}));
	Durin::Asset::FImportFactoryGraph Graph;
	std::vector<Durin::Asset::FImportDiagnostic> Diagnostics;
	ASSERT_TRUE(Builder.Finalize(Graph, Diagnostics));
	EXPECT_TRUE(Diagnostics.empty());
	ASSERT_EQ(Graph.GetNodes().size(), 2u);
	EXPECT_EQ(Graph.GetNodes()[0].StableIdentity, "output.material");
	EXPECT_EQ(Graph.GetNodes()[0].TranslatedNodeReferences,
		(std::vector<std::string>{"source.image", "source.material"}));
	EXPECT_FALSE(Graph.GetFingerprint().IsZero());
}

TEST(FInterchangeContractTests, FactoryGraphRejectsMissingTranslatedReference)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangeFactoryMissingReference");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const Durin::Asset::FTranslatedAssetGraph Translated = BuildTranslatedGraph(false);
	Durin::Asset::FImportFactoryGraphBuilder Builder(Translated);
	ASSERT_TRUE(Builder.AddNode({
		.StableIdentity = "output.texture",
		.FactoryId = "Durin.TextureFactory",
		.FactoryContractVersion = 1,
		.OutputClassName = "DTexture2D",
		.Destination = AssetPath("/InterchangeTests/Textures/Test"),
		.Settings = Payload("Durin.TextureFactory.Settings"),
		.TranslatedNodeReferences = {"source.unknown"}}));
	Durin::Asset::FImportFactoryGraph Graph;
	std::vector<Durin::Asset::FImportDiagnostic> Diagnostics;
	EXPECT_FALSE(Builder.Finalize(Graph, Diagnostics));
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_EQ(Diagnostics.back().Identity, "InterchangeMissingTranslatedReference");
}

TEST(FInterchangeContractTests, TypedPayloadAccessRejectsSchemaAndVersionMismatch)
{
	FTypedPayload Value;
	std::string Error;
	auto WrongSchema = Payload("Tests.OtherPayload", "value");
	WrongSchema.SchemaVersion = FTypedPayload::InterchangeSchemaVersion;
	ASSERT_TRUE(WrongSchema.Finalize(Error));
	EXPECT_FALSE(Durin::Asset::DecodeInterchangePayload(WrongSchema, Value, Error));
	EXPECT_NE(Error.find("InterchangeSchemaMismatch"), std::string::npos);

	auto WrongVersion = Payload(FTypedPayload::InterchangeSchemaId, "value");
	ASSERT_TRUE(WrongVersion.Finalize(Error));
	EXPECT_FALSE(Durin::Asset::DecodeInterchangePayload(WrongVersion, Value, Error));
	EXPECT_NE(Error.find("InterchangeSchemaVersionMismatch"), std::string::npos);

	WrongVersion.SchemaVersion = FTypedPayload::InterchangeSchemaVersion;
	ASSERT_TRUE(WrongVersion.Finalize(Error));
	EXPECT_TRUE(Durin::Asset::DecodeInterchangePayload(WrongVersion, Value, Error)) << Error;
	EXPECT_EQ(Value.Value, "value");
}

TEST(FInterchangeContractTests, ProvenanceRequiresExactFrameworkIdentity)
{
	Durin::Asset::FInterchangeProvenance Provenance;
	std::string Error;
	EXPECT_FALSE(Provenance.Validate(Error));
	EXPECT_NE(Error.find("InterchangeProvenanceIncomplete"), std::string::npos);

	Provenance.Translator.Id = "Tests.Translator";
	Provenance.Translator.ContractVersion = 1;
	Provenance.Sources.push_back({.StableIdentity = "root", .Role = "Root"});
	Provenance.TranslatedGraphFingerprint = Durin::FXxHash128::HashBuffer("translated");
	Provenance.FactoryGraphFingerprint = Durin::FXxHash128::HashBuffer("factory");
	Provenance.PipelineStack.push_back({
		.PipelineId = "Tests.DefaultPipeline", .ContractVersion = 1});
	EXPECT_TRUE(Provenance.Validate(Error)) << Error;
	Provenance.PipelineStack.back().ContractVersion = 0;
	EXPECT_FALSE(Provenance.Validate(Error));
	EXPECT_NE(Error.find("InterchangePipelineStackInvalid"), std::string::npos);
}

TEST(FInterchangeContractTests, RegistrySelectsPriorityAndReportsAmbiguity)
{
	auto& Service = Durin::Asset::GetImportService();
	std::string Error;
	auto Low = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.Low", .ContractVersion = 1},
			.Extensions = {".graph"},
			.Priority = 1},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Low) << Error;
	auto High = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.High", .ContractVersion = 1},
			.Extensions = {".graph"},
			.Priority = 2},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(High) << Error;

	const Durin::Asset::FImportSourceRecognition Source{.Extension = ".GRAPH"};
	auto Selected = Service.SelectTranslator(Source);
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected.Lease.GetId(), "Tests.Interchange.High");
	Selected = {};
	ASSERT_TRUE(High.Reset());

	auto Equal = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.Equal", .ContractVersion = 1},
			.Extensions = {".graph"},
			.Priority = 1},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Equal) << Error;
	Selected = Service.SelectTranslator(Source);
	EXPECT_FALSE(Selected);
	ASSERT_FALSE(Selected.Diagnostics.empty());
	EXPECT_EQ(Selected.Diagnostics.back().Category,
		Durin::Asset::EImportDiagnosticCategory::ProviderAmbiguous);
	Selected = {};
	EXPECT_TRUE(Equal.Reset());
	EXPECT_TRUE(Low.Reset());
}

TEST(FInterchangeContractTests, ExactRegistrationRetiresBeforeLeaseDestruction)
{
	auto& Service = Durin::Asset::GetImportService();
	const auto Destroyed = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	auto Registration = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.Lifetime", .ContractVersion = 1},
			.Extensions = {".lifetime"}},
		.Implementation = std::make_shared<FTestTranslator>(Destroyed)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;

	auto Lease = Service.FindInterchangeComponent(
		Durin::Asset::EInterchangeComponentRole::Translator,
		"Tests.Interchange.Lifetime", 1);
	ASSERT_TRUE(Lease);
	EXPECT_TRUE(Registration.Reset());
	EXPECT_FALSE(Service.FindInterchangeComponent(
		Durin::Asset::EInterchangeComponentRole::Translator,
		"Tests.Interchange.Lifetime", 1));
	EXPECT_FALSE(Destroyed->load(std::memory_order_acquire));
	Lease = {};
	EXPECT_TRUE(Destroyed->load(std::memory_order_acquire));
}

TEST(FInterchangeContractTests, ConcurrentLookupStopsAtExactRetirement)
{
	auto& Service = Durin::Asset::GetImportService();
	const auto Destroyed = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	auto Registration = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.Concurrent", .ContractVersion = 1},
			.Extensions = {".concurrent"}},
		.Implementation = std::make_shared<FTestTranslator>(Destroyed)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	std::atomic_bool Stop = false;
	std::atomic_uint64_t Lookups = 0;
	std::jthread Reader([&] {
		while (!Stop.load(std::memory_order_acquire))
		{
			auto Lease = Service.FindInterchangeComponent(
				Durin::Asset::EInterchangeComponentRole::Translator,
				"Tests.Interchange.Concurrent", 1);
			if (Lease) Lookups.fetch_add(1, std::memory_order_release);
		}
	});
	while (Lookups.load(std::memory_order_acquire) == 0) std::this_thread::yield();
	EXPECT_TRUE(Registration.Reset());
	EXPECT_FALSE(Service.FindInterchangeComponent(
		Durin::Asset::EInterchangeComponentRole::Translator,
		"Tests.Interchange.Concurrent", 1));
	Stop.store(true, std::memory_order_release);
	Reader.join();
	EXPECT_TRUE(Destroyed->load(std::memory_order_acquire));
}

TEST(FInterchangeContractTests, PersistedSelectionRequiresExactVersion)
{
	auto& Service = Durin::Asset::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.Versioned", .ContractVersion = 3},
			.Extensions = {".versioned"}},
		.Implementation = std::make_shared<FTestTranslator>()},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	const Durin::Asset::FImportSourceRecognition Source{.Extension = ".versioned"};
	EXPECT_TRUE(Service.SelectTranslator(Source, "Tests.Interchange.Versioned", 3));
	auto Mismatch = Service.SelectTranslator(Source, "Tests.Interchange.Versioned", 2);
	EXPECT_FALSE(Mismatch);
	ASSERT_FALSE(Mismatch.Diagnostics.empty());
	EXPECT_EQ(Mismatch.Diagnostics.back().Identity,
		"InterchangePersistedTranslatorUnavailable");
	Mismatch = {};
	EXPECT_TRUE(Registration.Reset());
}

TEST(FInterchangeContractTests, FactoryRegistrationSelectsByOutputClass)
{
	auto& Service = Durin::Asset::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterFactoryScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.TextureFactory", .ContractVersion = 4},
			.OutputClassName = "DTexture2D",
			.Priority = 5},
		.Implementation = std::make_shared<FTestFactory>()},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	auto Selected = Service.SelectFactory("DTexture2D");
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected.Lease.GetId(), "Tests.Interchange.TextureFactory");
	EXPECT_EQ(Selected.Lease.GetContractVersion(), 4u);
	EXPECT_EQ(Selected.Lease.GetOutputClassName(), "DTexture2D");
	EXPECT_EQ(Selected.Lease.GetThreadCapability(),
		Durin::Asset::EInterchangeThreadCapability::WorkerSafe);
	EXPECT_NE(Selected.Lease.GetFactory(), nullptr);
	auto Incompatible = Service.SelectFactory(
		"DMaterial", "Tests.Interchange.TextureFactory", 4);
	EXPECT_FALSE(Incompatible);
	ASSERT_FALSE(Incompatible.Diagnostics.empty());
	EXPECT_EQ(Incompatible.Diagnostics.back().Identity,
		"InterchangePersistedFactoryUnavailable");
	const auto Metadata = Service.EnumerateInterchangeComponents(
		Durin::Asset::EInterchangeComponentRole::Factory);
	ASSERT_EQ(Metadata.size(), 1u);
	EXPECT_EQ(Metadata.front().Id, "Tests.Interchange.TextureFactory");
	EXPECT_TRUE(Metadata.front().Settings.SchemaId.empty());
	Selected = {};
	EXPECT_TRUE(Registration.Reset());
}

TEST(FInterchangeContractTests, PipelineStackBuildsDeterministicFactoryGraph)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangePipelineStack");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::Asset::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterPipelineScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.Interchange.TexturePipeline", .ContractVersion = 1}},
		.Implementation = std::make_shared<FTestPipeline>(
			AssetPath("/InterchangeTests/Textures/Test"))},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	const auto Translated = BuildTranslatedGraph(false);
	const std::vector Stack = {Durin::Asset::FInterchangePipelineStackEntry{
		.PipelineId = "Tests.Interchange.TexturePipeline", .ContractVersion = 1}};
	const auto First = Durin::Asset::ExecuteInterchangePipelineStack(Translated, Stack);
	const auto Second = Durin::Asset::ExecuteInterchangePipelineStack(Translated, Stack);
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	EXPECT_EQ(First.Graph.GetFingerprint(), Second.Graph.GetFingerprint());
	ASSERT_EQ(First.Graph.GetNodes().size(), 1u);
	EXPECT_EQ(First.Graph.GetNodes().front().StableIdentity, "output.texture");
	EXPECT_TRUE(Registration.Reset());
}

TEST(FInterchangeContractTests, PipelineFailureDoesNotPublishPartialGraph)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangePipelineFailure");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::Asset::GetImportService();
	std::string Error;
	auto Registration = Service.RegisterPipelineScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.Interchange.FailingPipeline", .ContractVersion = 1}},
		.Implementation = std::make_shared<FTestPipeline>(
			AssetPath("/InterchangeTests/Textures/Failed"), false)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	const auto Translated = BuildTranslatedGraph(false);
	const std::vector Stack = {Durin::Asset::FInterchangePipelineStackEntry{
		.PipelineId = "Tests.Interchange.FailingPipeline", .ContractVersion = 1}};
	const auto Result = Durin::Asset::ExecuteInterchangePipelineStack(Translated, Stack);
	EXPECT_FALSE(Result);
	EXPECT_TRUE(Result.Graph.GetNodes().empty());
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Identity, "Tests.PipelineFailure");
	EXPECT_TRUE(Registration.Reset());
}

TEST(FInterchangeContractTests, PipelineExecutionRejectsRegistryRevisionChange)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangePipelineRevision");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::Asset::GetImportService();
	const auto Callback = std::make_shared<std::function<void()>>();
	std::string Error;
	auto Registration = Service.RegisterPipelineScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.Interchange.RevisionPipeline", .ContractVersion = 1}},
		.Implementation = std::make_shared<FTestPipeline>(
			AssetPath("/InterchangeTests/Textures/Stale"), true, Callback)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Registration) << Error;
	*Callback = [&] { EXPECT_TRUE(Registration.Reset()); };
	const auto Translated = BuildTranslatedGraph(false);
	const std::vector Stack = {Durin::Asset::FInterchangePipelineStackEntry{
		.PipelineId = "Tests.Interchange.RevisionPipeline", .ContractVersion = 1}};
	const auto Result = Durin::Asset::ExecuteInterchangePipelineStack(Translated, Stack);
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Identity, "InterchangeRegistryChanged");
}

TEST(FInterchangeContractTests, ProvenanceSerializationRoundTripsCanonicalValues)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangeProvenance");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	Durin::Asset::FInterchangeProvenance Provenance;
	std::string Error;
	Provenance.Translator = {
		.Id = "Tests.Translator",
		.ContractVersion = 2,
		.Settings = Payload("Tests.Translator.Settings", "translator")};
	ASSERT_TRUE(Provenance.Translator.Settings.Finalize(Error));
	auto PipelineSettings = Payload("Tests.Pipeline.Settings", "pipeline");
	ASSERT_TRUE(PipelineSettings.Finalize(Error));
	Provenance.PipelineStack.push_back({
		.PipelineId = "Tests.Pipeline", .ContractVersion = 3,
		.Settings = std::move(PipelineSettings)});
	Provenance.Sources.push_back({
		.StableIdentity = "root", .Role = "Root",
		.SourcePath = {.Path = "/InterchangeTests/source.graph"},
		.ContentHash = Durin::FXxHash128::HashBuffer("source"), .ByteCount = 6});
	Provenance.OutputMappings.push_back({
		.TranslatedNodeIdentity = "source.image",
		.OutputIdentity = "output.texture",
		.AssetPath = AssetPath("/InterchangeTests/Textures/Test")});
	Provenance.TranslatedGraphFingerprint = Durin::FXxHash128::HashBuffer("translated");
	Provenance.FactoryGraphFingerprint = Durin::FXxHash128::HashBuffer("factory");
	Provenance.AuthoredOutputFingerprint = "authored";
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::Asset::SerializeInterchangeProvenance(
		Provenance, Bytes, Error)) << Error;
	Durin::Asset::FInterchangeProvenance RoundTrip;
	ASSERT_TRUE(Durin::Asset::DeserializeInterchangeProvenance(
		Bytes, RoundTrip, Error)) << Error;
	EXPECT_EQ(RoundTrip, Provenance);
}

TEST(FInterchangeContractTests, UnifiedJobPublishesSyntheticGraphInlineAndScheduled)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangeUnifiedJob");
	WriteSource(Root / "source.graph", "graph\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::Asset::GetImportService();
	const auto TranslateCount = std::make_shared<std::atomic_uint32_t>(0);
	const auto BuildCount = std::make_shared<std::atomic_uint32_t>(0);
	const std::string ClassName = Durin::Asset::DImportRecord::StaticClass()
		->GetQualifiedName().ToString();
	std::string Error;
	auto Translator = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.JobTranslator", .ContractVersion = 1},
			.Extensions = {".graph"}},
		.Implementation = std::make_shared<FTestTranslator>(nullptr, TranslateCount)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Translator) << Error;
	auto Factory = Service.RegisterFactoryScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.JobFactory", .ContractVersion = 1},
			.OutputClassName = ClassName},
		.Implementation = std::make_shared<FJobFactory>(BuildCount)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Factory) << Error;
	const Durin::FAssetPath InlineFirst = AssetPath("/InterchangeTests/Inline/First");
	const Durin::FAssetPath InlineSecond = AssetPath("/InterchangeTests/Inline/Second");
	const Durin::FAssetPath ScheduledFirst = AssetPath("/InterchangeTests/Scheduled/First");
	const Durin::FAssetPath ScheduledSecond = AssetPath("/InterchangeTests/Scheduled/Second");
	const Durin::FAssetPath FailedFirst = AssetPath("/InterchangeTests/Failed/First");
	const Durin::FAssetPath FailedSecond = AssetPath("/InterchangeTests/Failed/Second");
	auto InlinePipeline = Service.RegisterPipelineScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.Interchange.InlinePipeline", .ContractVersion = 1}},
		.Implementation = std::make_shared<FJobPipeline>(
			InlineFirst, InlineSecond, ClassName)}, GetInterchangeTestGate(), Error);
	ASSERT_TRUE(InlinePipeline) << Error;
	auto ScheduledPipeline = Service.RegisterPipelineScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.Interchange.ScheduledPipeline", .ContractVersion = 1}},
		.Implementation = std::make_shared<FJobPipeline>(
			ScheduledFirst, ScheduledSecond, ClassName)}, GetInterchangeTestGate(), Error);
	ASSERT_TRUE(ScheduledPipeline) << Error;
	auto FailingPipeline = Service.RegisterPipelineScoped({
		.Descriptor = {.Identity = {
			.Id = "Tests.Interchange.FailingPublicationPipeline", .ContractVersion = 1}},
		.Implementation = std::make_shared<FJobPipeline>(
			FailedFirst, FailedSecond, ClassName)}, GetInterchangeTestGate(), Error);
	ASSERT_TRUE(FailingPipeline) << Error;

	Durin::Asset::FInterchangeImportRequest InlineRequest{
		.RootSource = {.Path = "/InterchangeTests/source.graph"},
		.TranslatorId = "Tests.Interchange.JobTranslator",
		.PipelineStack = {{
			.PipelineId = "Tests.Interchange.InlinePipeline", .ContractVersion = 1}},
		.Destination = InlineSecond,
		.Owner = {
			.OwnerId = "Tests.Interchange.InlineJob",
			.ConflictIdentities = {InlineFirst.ToString(), InlineSecond.ToString()}}};
	auto PreviewRequest = InlineRequest;
	PreviewRequest.Mode = Durin::Asset::EInterchangeImportMode::Preview;
	PreviewRequest.Lifetime = Durin::Asset::EImportOperationLifetime::EphemeralPreview;
	const Durin::Asset::FInterchangeImportResult Preview =
		Service.RunInterchangeImportInline(PreviewRequest, "Interchange preview test");
	ASSERT_EQ(Preview.Outcome.State, Durin::Asset::EImportOperationState::Succeeded);
	EXPECT_EQ(TranslateCount->load(std::memory_order_relaxed), 1u);
	EXPECT_EQ(BuildCount->load(std::memory_order_relaxed), 2u);
	const Durin::Asset::FInterchangeImportResult Inline =
		Service.RunInterchangeImportInline(InlineRequest, "Inline interchange test");
	ASSERT_EQ(Inline.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Inline.Outcome.Diagnostic;
	EXPECT_EQ(TranslateCount->load(std::memory_order_relaxed), 1u);
	EXPECT_EQ(BuildCount->load(std::memory_order_relaxed), 2u);
	EXPECT_EQ(Inline.Outcome.PublishedAssetIdentities.size(), 2u);
	EXPECT_EQ(Inline.Provenance.OutputMappings.size(), 2u);
	EXPECT_TRUE(Durin::Asset::FindAssetExact(InlineFirst));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(InlineSecond));

	Durin::Asset::FInterchangeImportRequest ScheduledRequest{
		.RootSource = {.Path = "/InterchangeTests/source.graph"},
		.TranslatorId = "Tests.Interchange.JobTranslator",
		.PipelineStack = {{
			.PipelineId = "Tests.Interchange.ScheduledPipeline", .ContractVersion = 1}},
		.Destination = ScheduledSecond,
		.Owner = {
			.OwnerId = "Tests.Interchange.ScheduledJob",
			.ConflictIdentities = {ScheduledFirst.ToString(), ScheduledSecond.ToString()}}};
	const auto Handle = Service.SubmitInterchangeImport(
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
	Durin::Asset::FInterchangeImportResult Scheduled;
	ASSERT_TRUE(Handle.TryGetResult(Scheduled));
	ASSERT_EQ(Scheduled.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Scheduled.Outcome.Diagnostic;
	EXPECT_EQ(Scheduled.Outcome.PublishedAssetIdentities.size(), 2u);
	EXPECT_EQ(Scheduled.Provenance.TranslatedGraphFingerprint,
		Inline.Provenance.TranslatedGraphFingerprint);
	EXPECT_TRUE(Durin::Asset::FindAssetExact(ScheduledFirst));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(ScheduledSecond));

	Durin::Asset::FInterchangeImportRequest FailedRequest{
		.RootSource = {.Path = "/InterchangeTests/source.graph"},
		.TranslatorId = "Tests.Interchange.JobTranslator",
		.PipelineStack = {{
			.PipelineId = "Tests.Interchange.FailingPublicationPipeline",
			.ContractVersion = 1}},
		.Destination = FailedSecond,
		.Owner = {
			.OwnerId = "Tests.Interchange.FailedJob",
			.ConflictIdentities = {FailedFirst.ToString(), FailedSecond.ToString()}},
		.SaveOptions = {.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
			return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
		}}};
	const auto Failed = Service.RunInterchangeImportInline(
		FailedRequest, "Failed interchange publication test");
	EXPECT_EQ(Failed.Outcome.State, Durin::Asset::EImportOperationState::Failed);
	EXPECT_FALSE(Durin::Asset::FindAssetExact(FailedFirst));
	EXPECT_FALSE(Durin::Asset::FindAssetExact(FailedSecond));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(FailedFirst), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(FailedSecond), nullptr);

	EXPECT_TRUE(Durin::Asset::UnloadPackage(InlineFirst));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(InlineSecond));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(ScheduledFirst));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(ScheduledSecond));
	EXPECT_TRUE(FailingPipeline.Reset());
	EXPECT_TRUE(ScheduledPipeline.Reset());
	EXPECT_TRUE(InlinePipeline.Reset());
	EXPECT_TRUE(Factory.Reset());
	EXPECT_TRUE(Translator.Reset());
}

TEST(FInterchangeContractTests, UnifiedJobCancellationProducesOneTerminalOutcome)
{
	Durin::ShutdownTaskScheduler(false);
	FTaskSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("InterchangeJobCancellation");
	WriteSource(Root / "source.block", "block\n");
	const std::array Mounts = {MakeMount(Root)};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	auto& Service = Durin::Asset::GetImportService();
	const auto Entered = std::make_shared<std::atomic_bool>(false);
	std::string Error;
	auto Translator = Service.RegisterTranslatorScoped({
		.Descriptor = {
			.Identity = {.Id = "Tests.Interchange.BlockingTranslator", .ContractVersion = 1},
			.Extensions = {".block"}},
		.Implementation = std::make_shared<FBlockingTranslator>(Entered)},
		GetInterchangeTestGate(), Error);
	ASSERT_TRUE(Translator) << Error;
	const auto Handle = Service.SubmitInterchangeImport({
		.RootSource = {.Path = "/InterchangeTests/source.block"},
		.TranslatorId = "Tests.Interchange.BlockingTranslator",
		.PipelineStack = {{.PipelineId = "Tests.Unreached", .ContractVersion = 1}},
		.Owner = {.OwnerId = "Tests.Interchange.CancelJob"}},
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
	Durin::Asset::FInterchangeImportResult Result;
	ASSERT_TRUE(Handle.TryGetResult(Result));
	EXPECT_EQ(Result.Outcome.State, Durin::Asset::EImportOperationState::Canceled);
	Durin::Asset::FImportOutcome Second;
	ASSERT_TRUE(Handle.GetOperationHandle().TryGetOutcome(Second));
	EXPECT_EQ(Second, Result.Outcome);
	EXPECT_TRUE(Translator.Reset());
}

static_assert(std::is_move_constructible_v<Durin::Asset::FTranslatedAssetGraph>);
static_assert(std::is_move_constructible_v<Durin::Asset::FImportFactoryGraph>);
