#include "ImageImportProviderCommon.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	class FVolumePlanningPass final : public IPlanningPass
		{
		public:
			auto Execute(const FSourceGraph& Graph, const FBuildGraph*,
				const FSchemaPayload& Settings, FBuildGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				FVolumePlan Plan;
				std::string Error;
				if (!Graph.FindNode("image") || !DecodeVolumePlan(Settings, Plan, Error))
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.PlanInvalid", "PlanningPass", Error);
					return false;
				}
				return Builder.AddNode({.StableIdentity = "volume-texture",
					.BuilderId = std::string(VolumeBuilderId), .BuilderContractVersion = 1,
					.OutputClassName = "Durin::DVolumeTexture", .Destination = Plan.Destination,
					.Policy = Plan.Policy, .Settings = Settings,
					.SourceNodeReferences = {"image"}});
			}
		};

		class FVolumeProduct final : public IBuildProduct
		{
		public:
			Asset::Build::FVolumeTextureBuildProduct Product;
			FVolumeTextureSourceImportData Provenance;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IBuildProduct> override
			{
				auto Result = std::make_unique<FVolumeProduct>();
				Result->Product.SourceData = Product.SourceData;
				Result->Product.Settings = Product.Settings;
				if (Product.PlatformData)
					Result->Product.PlatformData =
						std::make_unique<FVolumeTexturePlatformData>(*Product.PlatformData);
				Result->Product.DerivedDataKey = Product.DerivedDataKey;
				Result->Product.bCacheHit = Product.bCacheHit;
				Result->Provenance = Provenance;
				return Result;
			}
		};

		class FVolumeAssetBuilder final : public IAssetBuilder
		{
		public:
			auto BuildDetachedProduct(const FBuildNode& Node,
				const FSourceGraph& Graph, IImportProgressReporter*,
				const std::function<bool()>& Canceled,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IBuildProduct> override
			{
				if (Canceled()) return {};
				const auto* ImageNode = Graph.FindNode("image");
				FDecodedImage Image;
				FVolumePlan Plan;
				std::string Error;
				if (!ImageNode || !DecodeImage(ImageNode->Payload, Image, Error)
					|| !DecodeVolumePlan(Node.Settings, Plan, Error)) return {};
				const uint64 ExpectedWidth = static_cast<uint64>(Plan.Settings.SliceWidth) * Plan.Settings.TilesX;
				const uint64 ExpectedHeight = static_cast<uint64>(Plan.Settings.SliceHeight) * Plan.Settings.TilesY;
				if (Image.Source.Width != ExpectedWidth || Image.Source.Height != ExpectedHeight)
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.LayoutMismatch", "ProductBuild",
						std::format("Volume atlas is {}x{}; expected {}x{}.", Image.Source.Width,
							Image.Source.Height, ExpectedWidth, ExpectedHeight));
					return {};
				}
				const uint32 BytesPerVoxel = Plan.Settings.GetOutputFormat() == EVolumeTextureFormat::RGBA8_UNORM ? 4 : 1;
				std::vector<std::byte> Voxels;
				Voxels.reserve(static_cast<size_t>(Plan.Settings.SliceWidth) * Plan.Settings.SliceHeight
					* Plan.Settings.Depth * BytesPerVoxel);
				auto AppendPixel = [&](size_t Pixel) {
					if (Plan.Settings.Channels == EVolumeTextureSourceChannels::RGBA)
					{
						Voxels.insert(Voxels.end(), Image.Source.Pixels.begin() + Pixel,
							Image.Source.Pixels.begin() + Pixel + 4);
						return;
					}
					uint8 Value = 0;
					switch (Plan.Settings.Channels)
					{
					case EVolumeTextureSourceChannels::Red: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel]); break;
					case EVolumeTextureSourceChannels::Green: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel + 1]); break;
					case EVolumeTextureSourceChannels::Blue: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel + 2]); break;
					case EVolumeTextureSourceChannels::Alpha: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel + 3]); break;
					case EVolumeTextureSourceChannels::Luminance:
						Value = static_cast<uint8>((54 * std::to_integer<uint8>(Image.Source.Pixels[Pixel])
							+ 183 * std::to_integer<uint8>(Image.Source.Pixels[Pixel + 1])
							+ 19 * std::to_integer<uint8>(Image.Source.Pixels[Pixel + 2]) + 128) >> 8); break;
					case EVolumeTextureSourceChannels::RGBA: break;
					}
					Voxels.push_back(static_cast<std::byte>(Value));
				};
				for (uint32 Z = 0; Z < Plan.Settings.Depth; ++Z)
					for (uint32 Y = 0; Y < Plan.Settings.SliceHeight; ++Y)
						for (uint32 X = 0; X < Plan.Settings.SliceWidth; ++X)
						{
							const uint32 TileX = Z % Plan.Settings.TilesX;
							const uint32 TileY = Z / Plan.Settings.TilesX;
							const size_t Pixel = (static_cast<size_t>(TileY * Plan.Settings.SliceHeight + Y)
								* Image.Source.Width + TileX * Plan.Settings.SliceWidth + X) * 4;
							AppendPixel(Pixel);
						}
				FVolumeTextureSourceData Source{.Width = Plan.Settings.SliceWidth,
					.Height = Plan.Settings.SliceHeight, .Depth = Plan.Settings.Depth,
					.Format = Plan.Settings.GetOutputFormat()};
				if (!Source.SetVoxelBytes(Voxels)) return {};
				auto Result = std::make_unique<FVolumeProduct>();
				if (!Asset::Build::BuildVolumeTexture(std::move(Source),
					{.OutputFormat = Plan.Settings.GetOutputFormat()}, Result->Product, Error))
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.BuildFailed", "ProductBuild", Error);
					return {};
				}
				Result->Provenance = {.Source = {.SourcePath = Image.Path,
					.SourceContentHashLow = Image.Hash.HashLow, .SourceContentHashHigh = Image.Hash.HashHigh},
					.SourceFile = Image.Path.Path, .ImportFormat = Plan.Settings.ImportFormat,
					.Channels = Plan.Settings.Channels, .SliceWidth = Plan.Settings.SliceWidth,
					.SliceHeight = Plan.Settings.SliceHeight, .Depth = Plan.Settings.Depth,
					.TilesX = Plan.Settings.TilesX, .TilesY = Plan.Settings.TilesY,
					.DecoderId = std::string(VolumeTextureSourceProviderId),
					.DecoderVersion = VolumeTextureSourceProviderVersion};
				return Result;
			}
			auto MaterializeCandidate(const FBuildNode& Node,
				std::unique_ptr<IBuildProduct> Product,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Typed = dynamic_cast<FVolumeProduct*>(Product.get());
				FAssetPath Path = Node.Destination;
				if (Node.Policy != EImportOutputPolicy::Create && !MakeCandidatePath(Node.Destination, Path)) return {};
				DVolumeTexture* AssetObject = nullptr;
				if (!Typed || !Asset::CreateAsset(Path, AssetObject)) return {};
				auto Result = std::make_unique<FBuiltinSingleAssetCandidate>(
					AssetObject, Node.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::PublishVolumeTextureProduct(*AssetObject,
					std::move(Typed->Product), Error)
					|| !AssetObject->PublishSourceImportData(std::move(Typed->Provenance), Error))
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.MaterializeFailed", "Materialization", Error);
					Result->Abandon();
					return {};
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* A = Cast<DVolumeTexture>(&Target);
				auto* B = Cast<DVolumeTexture>(Candidate.GetAsset());
				return A && B
					? std::make_unique<TImportedStateExchange<DVolumeTexture>>(*A, *B)
					: nullptr;
			}
			auto ApplyProvenance(DObject& Object, const FImportProvenance& Provenance,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				return ApplyProvenanceBytes(Object, Provenance, Diagnostics);
			}
		};

		}

	auto MakeVolumeTextureImportRequest(const FSourcePath& MountedSource,
		const FAssetPath& Destination, const FVolumeTextureImportSettings& Settings,
		EImportMode Mode, FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest, std::string& OutError,
		const FVolumeTextureAuthoringOptions& AuthoringOptions) -> bool
	{
		if (MountedSource.IsEmpty() || Destination.ToString().empty() || !Settings.IsValid(&OutError)) return false;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "VolumeTexture.AssetForge";
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(Destination.ToString());
		OutRequest = {.Mode = Mode, .RootSource = MountedSource,
			.TranslatorId = std::string(ImageTranslatorId),
			.TranslatorSettings = EmptyPayload(EmptyImageSettingsSchema),
			.PlanningPassStack = {{.PlanningPassId = std::string(VolumePlanningPassId), .ContractVersion = 1,
				.Settings = EncodeVolumePlan({.Destination = Destination, .Policy = PolicyFor(Mode),
					.Settings = Settings})}}, .Destination = Destination, .Owner = std::move(Owner),
			.SaveOptions = {.WriterSelection = AuthoringOptions.WriterSelection},
			.ExistingProvenance = std::move(ExistingProvenance)};
		OutError.clear();
		return true;
	}

	auto InspectVolumeTextureImportProvenance(const DVolumeTexture& Texture,
		FImportProvenance& Out, std::string& OutError) -> bool
	{
		if (!Texture.GetImportProvenance().empty())
			return DecodeStoredImportProvenance(
				Texture.GetImportProvenance(), Out, OutError);
		OutError = "VolumeTexture has no persisted AssetForge provenance; reimport requires explicit repair.";
		return false;
	}

	auto RegisterVolumeTextureImports(FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool
	{
		auto Add = [&](FComponentRegistration Registration) {
			if (!Registration) return false;
			OutRegistrations.push_back(std::move(Registration));
			return true;
		};
		if (!Add(Service.RegisterPlanningPassScoped({.Descriptor = {
			.Identity = {.Id = std::string(VolumePlanningPassId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(VolumePlanSchema), .SchemaVersion = 1}},
			.Priority = 100, .ExecutionThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FVolumePlanningPass>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterAssetBuilderScoped({.Descriptor = {
			.Identity = {.Id = std::string(VolumeBuilderId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(VolumePlanSchema), .SchemaVersion = 1}},
			.OutputClassName = "Durin::DVolumeTexture", .Priority = 100,
			.ProductBuildThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FVolumeAssetBuilder>()}, OwnerGate, OutError))) return false;
		return true;
	}}
