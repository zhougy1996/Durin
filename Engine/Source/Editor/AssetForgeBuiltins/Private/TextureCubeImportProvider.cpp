#include "ImageImportProviderCommon.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	class FCubeTranslator final : public ISourceTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				return IsTextureCubePanoramaSourceExtension(Source.Extension);
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>,
				FDependencyRequestSink&, std::vector<FImportDiagnostic>&) const -> bool override { return true; }
			auto Translate(const FSourceSnapshot& Snapshot, const FSchemaPayload& Settings,
				FSourceGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				ETextureCubeSourceLayout Layout;
				std::string Error;
				if (!DecodeCubeTranslatorSettings(Settings, Layout, Error))
				{
					AddFailure(Diagnostics, "Durin.TextureCube.SettingsInvalid", "Translation", Error);
					return false;
				}
				if (Layout == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
					FTextureCubePanoramaSourceData Panorama;
					if (!Root || !TranslateTextureCubePanoramaSource(Root->GetBytes(),
						std::filesystem::path(Root->SourcePath.Path).extension().generic_string(), Panorama, Error))
					{
						AddFailure(Diagnostics, "Durin.TextureCube.DecodeFailed", "Translation",
							std::format("Panorama decode failed: {}", Error));
						return false;
					}
					return Builder.AddNode({.StableIdentity = "panorama",
						.NodeKind = "Durin.TextureCube.Panorama",
						.Payload = EncodePanorama(Panorama, *Root),
						.SourceIdentities = {"root"}});
				}
				for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					const std::string SourceId = Index == 0 ? "root" : std::format("face:{}", Index);
					const FSourceSnapshotEntry* Entry = Snapshot.FindSource(SourceId);
					FDecodedImage Image;
					if (!Entry || !TranslateTexture2DSource(Entry->GetBytes(), Image.Source, Error))
					{
						AddFailure(Diagnostics, "Durin.TextureCube.FaceDecodeFailed", "Translation", Error);
						return false;
					}
					Image.Path = Entry->SourcePath;
					Image.Hash = Entry->ContentHash;
					Image.FileSize = Entry->ByteCount;
					Image.LastWriteTime = Entry->LastWriteTime;
					if (!Builder.AddNode({.StableIdentity = std::format("image:{}", Index),
						.NodeKind = "Durin.Image.RGBA8", .Payload = EncodeImage(Image),
						.SourceIdentities = {SourceId}})) return false;
				}
				return true;
			}
		};

		class FCubePlanningPass final : public IPlanningPass
		{
		public:
			auto Execute(const FSourceGraph& Graph, const FBuildGraph*,
				const FSchemaPayload& Settings, FBuildGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				FCubePlan Plan;
				std::string Error;
				if (!DecodeCubePlan(Settings, Plan, Error)
					|| (Plan.Layout == ETextureCubeSourceLayout::SixFaces
						? !Graph.FindNode("image:0") : !Graph.FindNode("panorama")))
				{
					AddFailure(Diagnostics, "Durin.TextureCube.PlanInvalid", "PlanningPass", Error);
					return false;
				}
				std::vector<std::string> References;
				if (Plan.Layout == ETextureCubeSourceLayout::SixFaces)
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
						References.push_back(std::format("image:{}", Index));
				else References.push_back("panorama");
				return Builder.AddNode({.StableIdentity = "texture-cube",
					.BuilderId = std::string(CubeBuilderId), .BuilderContractVersion = 1,
					.OutputClassName = "Durin::DTextureCube", .Destination = Plan.Destination,
					.Policy = Plan.Policy, .Settings = Settings,
					.SourceNodeReferences = std::move(References)});
			}
		};

		class FCubeProduct final : public IBuildProduct
		{
		public:
			Asset::Build::FTextureCubeBuildProduct Product;
			Asset::Build::FTextureCubePublicationContext Publication;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IBuildProduct> override
			{
				auto Result = std::make_unique<FCubeProduct>();
				Result->Product.SourceLayout = Product.SourceLayout;
				Result->Product.SourceData = Product.SourceData;
				if (Product.PlatformData)
					Result->Product.PlatformData =
						std::make_unique<FTextureCubePlatformData>(*Product.PlatformData);
				Result->Product.DerivedDataKey = Product.DerivedDataKey;
				Result->Product.SourceWidth = Product.SourceWidth;
				Result->Product.SourceHeight = Product.SourceHeight;
				Result->Product.PanoramaFaceDimension = Product.PanoramaFaceDimension;
				Result->Product.PanoramaExposureEV = Product.PanoramaExposureEV;
				Result->Product.bSRGB = Product.bSRGB;
				Result->Publication = Publication;
				return Result;
			}
		};

		class FCubeAssetBuilder final : public IAssetBuilder
		{
		public:
			auto BuildDetachedProduct(const FBuildNode& Node,
				const FSourceGraph& Graph, IImportProgressReporter*,
				const std::function<bool()>& Canceled,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IBuildProduct> override
			{
				if (Canceled()) return {};
				FCubePlan Plan;
				std::string Error;
				if (!DecodeCubePlan(Node.Settings, Plan, Error)) return {};
				auto Result = std::make_unique<FCubeProduct>();
				bool Built = false;
				if (Plan.Layout == ETextureCubeSourceLayout::SixFaces)
				{
					FTextureCubeSourceData Source;
					std::array<FXxHash128, TextureCubeFaceCount> Hashes;
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
					{
						const auto* ImageNode = Graph.FindNode(std::format("image:{}", Index));
						FDecodedImage Image;
						if (!ImageNode || !DecodeImage(ImageNode->Payload, Image, Error)) return {};
						Source.Faces[Index] = std::move(Image.Source);
						Hashes[Index] = Image.Hash;
						Result->Publication.FaceHashes[Index] = Image.Hash;
						Result->Publication.FacePaths[Index] = std::move(Image.Path);
					}
					Built = Asset::Build::BuildTextureCubeFaces(std::move(Source), Hashes,
						Plan.FaceSettings, Result->Product, Error);
				}
				else
				{
					const auto* PanoramaNode = Graph.FindNode("panorama");
					FDecodedPanorama Panorama;
					if (!PanoramaNode || !DecodePanorama(PanoramaNode->Payload, Panorama, Error)) return {};
					Result->Publication.PanoramaHash = Panorama.Hash;
					Result->Publication.PanoramaPath = std::move(Panorama.Path);
					Built = std::visit([&](auto&& Source) {
						return Asset::Build::BuildTextureCubePanorama(std::move(Source), Panorama.Hash,
							Plan.PanoramaSettings, Result->Product, Error);
					}, std::move(Panorama.Source));
				}
				if (!Built)
				{
					AddFailure(Diagnostics, "Durin.TextureCube.BuildFailed", "ProductBuild", Error);
					return {};
				}
				return Result;
			}

			auto MaterializeCandidate(const FBuildNode& Node,
				std::unique_ptr<IBuildProduct> Product,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Typed = dynamic_cast<FCubeProduct*>(Product.get());
				FAssetPath Path = Node.Destination;
				if (Node.Policy != EImportOutputPolicy::Create && !MakeCandidatePath(Node.Destination, Path)) return {};
				DTextureCube* AssetObject = nullptr;
				if (!Typed || !Asset::CreateAsset(Path, AssetObject)) return {};
				auto Result = std::make_unique<FBuiltinSingleAssetCandidate>(
					AssetObject, Node.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::PublishTextureCubeProduct(*AssetObject,
					std::move(Typed->Product), Typed->Publication, Error))
				{
					AddFailure(Diagnostics, "Durin.TextureCube.MaterializeFailed", "Materialization", Error);
					Result->Abandon();
					return {};
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* A = Cast<DTextureCube>(&Target);
				auto* B = Cast<DTextureCube>(Candidate.GetAsset());
				return A && B
					? std::make_unique<TImportedStateExchange<DTextureCube>>(*A, *B)
					: nullptr;
			}
			auto ApplyProvenance(DObject& Object, const FImportProvenance& Provenance,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				return ApplyProvenanceBytes(Object, Provenance, Diagnostics);
			}
		};

		}

	auto MakeTextureCubeImportRequest(std::span<const FSourcePath> MountedSources,
		ETextureCubeSourceLayout Layout, const FAssetPath& Destination,
		const FTextureCubeImportSettings& FaceSettings,
		const FTextureCubePanoramaImportSettings& PanoramaSettings,
		EImportMode Mode, FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest, std::string& OutError) -> bool
	{
		const size_t Required = Layout == ETextureCubeSourceLayout::SixFaces ? TextureCubeFaceCount : 1;
		if (MountedSources.size() != Required || Destination.ToString().empty()
			|| std::ranges::any_of(MountedSources, [](const FSourcePath& Source) { return Source.IsEmpty(); }))
		{
			OutError = "TextureCube AssetForge source set is invalid.";
			return false;
		}
		if (Owner.OwnerId.empty()) Owner.OwnerId = "TextureCube.AssetForge";
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(Destination.ToString());
		const FCubePlan Plan{.Destination = Destination, .Policy = PolicyFor(Mode), .Layout = Layout,
			.FaceSettings = FaceSettings, .PanoramaSettings = PanoramaSettings};
		OutRequest = {.Mode = Mode, .RootSource = MountedSources.front(),
			.TranslatorId = std::string(CubeTranslatorId),
			.TranslatorSettings = EncodeCubeTranslatorSettings(Layout),
			.PlanningPassStack = {{.PlanningPassId = std::string(CubePlanningPassId),
				.ContractVersion = 1, .Settings = EncodeCubePlan(Plan)}},
			.Destination = Destination, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		for (size_t Index = 1; Index < MountedSources.size(); ++Index)
			OutRequest.DeclaredSources.push_back({.StableIdentity = std::format("face:{}", Index),
				.Role = std::format("CubeFace{}", Index), .SourcePath = MountedSources[Index]});
		OutError.clear();
		return true;
	}

	auto InspectTextureCubeImportProvenance(const DTextureCube& Texture,
		FImportProvenance& Out, std::string& OutError) -> bool
	{
		if (!Texture.GetImportProvenance().empty())
			return DecodeStoredImportProvenance(
				Texture.GetImportProvenance(), Out, OutError);
		OutError = "TextureCube has no persisted AssetForge provenance; reimport requires explicit repair.";
		return false;
	}

	auto RegisterTextureCubeImports(FImportService& Service,
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
			.Identity = {.Id = std::string(CubeTranslatorId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(CubeTranslatorSchema), .SchemaVersion = 1}},
			.Extensions = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"}, .Priority = 120,
			.TranslationThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FCubeTranslator>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterPlanningPassScoped({.Descriptor = {
			.Identity = {.Id = std::string(CubePlanningPassId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(CubePlanSchema), .SchemaVersion = 1}},
			.Priority = 100, .ExecutionThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FCubePlanningPass>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterAssetBuilderScoped({.Descriptor = {
			.Identity = {.Id = std::string(CubeBuilderId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(CubePlanSchema), .SchemaVersion = 1}},
			.OutputClassName = "Durin::DTextureCube", .Priority = 100,
			.ProductBuildThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FCubeAssetBuilder>()}, OwnerGate, OutError))) return false;
		return true;
	}}
