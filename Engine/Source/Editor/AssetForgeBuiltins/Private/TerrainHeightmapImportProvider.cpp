#include "ImageImportProviderCommon.h"
#include "Asset/AssetAuthoringOperations.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	class FTerrainTranslator final : public ISourceTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				return IsTerrainHeightmapSourceExtension(Source.Extension);
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>, FDependencyRequestSink&,
				std::vector<FImportDiagnostic>&) const -> bool override { return true; }
			auto Translate(const FSourceSnapshot& Snapshot, const FSchemaPayload& Settings,
				FSourceGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				std::string Error;
				if (!ValidatePayload(Settings, TerrainTranslatorSchema, Error)) return false;
				const auto* Root = Snapshot.FindSource("root");
				FDecodedTerrain Terrain;
				if (!Root || !TranslateTerrainHeightmapSource(
					std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
					Root->GetBytes(), Terrain.Source, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.DecodeFailed", "Translation", Error);
					return false;
				}
				Terrain.Path = Root->SourcePath;
				Terrain.Hash = Root->ContentHash;
				Terrain.FileSize = Root->ByteCount;
				Terrain.LastWriteTime = Root->LastWriteTime;
				return Builder.AddNode({.StableIdentity = "heightmap",
					.NodeKind = "Durin.TerrainHeightmap.Samples",
					.Payload = EncodeTerrain(Terrain), .SourceIdentities = {"root"}});
			}
		};

		class FTerrainPlanningPass final : public IPlanningPass
		{
		public:
			auto Execute(const FSourceGraph& Graph, const FBuildGraph*,
				const FSchemaPayload& Settings, FBuildGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				FTerrainPlan Plan;
				std::string Error;
				if (!Graph.FindNode("heightmap") || !DecodeTerrainPlan(Settings, Plan, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.PlanInvalid", "PlanningPass", Error);
					return false;
				}
				return Builder.AddNode({.StableIdentity = "terrain-heightmap",
					.BuilderId = std::string(TerrainBuilderId), .BuilderContractVersion = 1,
					.OutputClassName = "Durin::DTerrainHeightmap", .Destination = Plan.Destination,
					.Policy = Plan.Policy, .Settings = Settings,
					.SourceNodeReferences = {"heightmap"}});
			}
		};

		class FTerrainProduct final : public IBuildProduct
		{
		public:
			Asset::FTerrainHeightmapBuildProduct Product;
			Asset::FTerrainHeightmapPublicationContext Publication;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IBuildProduct> override
			{
				return std::make_unique<FTerrainProduct>(*this);
			}
		};

		class FTerrainAssetBuilder final : public IAssetBuilder
		{
		public:
			auto BuildDetachedProduct(const FBuildNode& Node,
				const FSourceGraph& Graph, IImportProgressReporter*,
				const std::function<bool()>& Canceled,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IBuildProduct> override
			{
				if (Canceled()) return {};
				const auto* SourceNode = Graph.FindNode("heightmap");
				FDecodedTerrain Terrain;
				FTerrainPlan Plan;
				std::string Error;
				if (!SourceNode || !DecodeTerrain(SourceNode->Payload, Terrain, Error)
					|| !DecodeTerrainPlan(Node.Settings, Plan, Error)) return {};
				auto Result = std::make_unique<FTerrainProduct>();
				if (!Asset::BuildTerrainHeightmap({.Samples = std::move(Terrain.Source.Samples),
					.Width = Terrain.Source.Width, .Height = Terrain.Source.Height,
					.SourceContentHashLow = Terrain.Hash.HashLow,
					.SourceContentHashHigh = Terrain.Hash.HashHigh,
					.DecoderId = Terrain.Source.DecoderId,
					.DecoderVersion = Terrain.Source.DecoderVersion,
					.SourceFormat = Terrain.Source.SourceFormat,
					.SourceProfileVersion = Terrain.Source.SourceProfileVersion,
					.ShouldCancel = Canceled}, Result->Product, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.BuildFailed", "ProductBuild", Error);
					return {};
				}
				Result->Publication = {.SourcePath = std::move(Terrain.Path),
					.DecoderId = std::move(Terrain.Source.DecoderId),
					.DecoderVersion = Terrain.Source.DecoderVersion,
					.SourceFormat = Terrain.Source.SourceFormat,
					.SourceProfileVersion = Terrain.Source.SourceProfileVersion,
					.SourceFileSize = Terrain.FileSize,
					.SourceLastWriteTime = Terrain.LastWriteTime};
				return Result;
			}
			auto MaterializeCandidate(const FBuildNode& Node,
				std::unique_ptr<IBuildProduct> Product,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Typed = dynamic_cast<FTerrainProduct*>(Product.get());
				FAssetPath Path = Node.Destination;
				if (Node.Policy != EImportOutputPolicy::Create && !MakeCandidatePath(Node.Destination, Path)) return {};
				DTerrainHeightmap* AssetObject = nullptr;
				if (!Typed || !Asset::CreateAsset(Path, AssetObject)) return {};
				auto Result = std::make_unique<FBuiltinSingleAssetCandidate>(
					AssetObject, Node.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::PublishTerrainHeightmapProduct(*AssetObject,
					std::move(Typed->Product), Typed->Publication, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.MaterializeFailed", "Materialization", Error);
					Result->Abandon();
					return {};
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* A = Cast<DTerrainHeightmap>(&Target);
				auto* B = Cast<DTerrainHeightmap>(Candidate.GetAsset());
				if (A && B) A->PrepareCandidateRevision(*B);
				return A && B
					? std::make_unique<TImportedStateExchange<DTerrainHeightmap>>(*A, *B)
					: nullptr;
			}
			auto ApplyProvenance(DObject& Object, const FImportProvenance& Provenance,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				return ApplyProvenanceBytes(Object, Provenance, Diagnostics);
			}
		};

		}

	auto MakeTerrainHeightmapImportRequest(const FSourcePath& MountedSource,
		const FAssetPath& Destination, EImportMode Mode, FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest, std::string& OutError) -> bool
	{
		if (MountedSource.IsEmpty() || Destination.ToString().empty())
		{
			OutError = "Terrain heightmap AssetForge request is invalid.";
			return false;
		}
		if (Owner.OwnerId.empty()) Owner.OwnerId = "TerrainHeightmap.AssetForge";
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(Destination.ToString());
		OutRequest = {.Mode = Mode, .RootSource = MountedSource,
			.TranslatorId = std::string(TerrainTranslatorId),
			.TranslatorSettings = EmptyPayload(TerrainTranslatorSchema),
			.PlanningPassStack = {{.PlanningPassId = std::string(TerrainPlanningPassId), .ContractVersion = 1,
				.Settings = EncodeTerrainPlan({.Destination = Destination, .Policy = PolicyFor(Mode)})}},
			.Destination = Destination, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		OutError.clear();
		return true;
	}

	auto InspectTerrainHeightmapImportProvenance(const DTerrainHeightmap& Heightmap,
		FImportProvenance& Out, std::string& OutError) -> bool
	{
		if (!Heightmap.GetImportProvenance().empty())
			return DecodeStoredImportProvenance(
				Heightmap.GetImportProvenance(), Out, OutError);
		OutError = "Terrain heightmap has no persisted AssetForge provenance; reimport requires explicit repair.";
		return false;
	}

	auto RegisterTerrainHeightmapImports(FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool
	{
		auto Add = [&](FComponentRegistration Registration) {
			if (!Registration) return false;
			OutRegistrations.push_back(std::move(Registration));
			return true;
		};
		if (!Add(Service.RegisterSourceTranslatorScoped({.Descriptor = {
			.Identity = {.Id = std::string(TerrainTranslatorId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(TerrainTranslatorSchema), .SchemaVersion = 1}},
			.Extensions = {".png", ".raw", ".r16"}, .Priority = 120,
			.TranslationThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTerrainTranslator>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterPlanningPassScoped({.Descriptor = {
			.Identity = {.Id = std::string(TerrainPlanningPassId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(TerrainPlanSchema), .SchemaVersion = 1}},
			.Priority = 100, .ExecutionThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTerrainPlanningPass>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterAssetBuilderScoped({.Descriptor = {
			.Identity = {.Id = std::string(TerrainBuilderId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(TerrainPlanSchema), .SchemaVersion = 1}},
			.OutputClassName = "Durin::DTerrainHeightmap", .Priority = 100,
			.ProductBuildThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTerrainAssetBuilder>()}, OwnerGate, OutError))) return false;
		return true;
	}}
