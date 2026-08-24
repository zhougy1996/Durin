#include "Texture2DImportProviderSchema.h"
#include "BuiltinProviderRegistration.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	class FImageSourceTranslator final : public ISourceTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				return IsTexture2DSourceExtension(Source.Extension);
			}
			auto DiscoverDependencies(
				std::span<const FSourceSnapshotEntry>, FDependencyRequestSink&,
				std::vector<FImportDiagnostic>&) const -> bool override { return true; }
			auto Translate(
				const FSourceSnapshot& Snapshot,
				const FSchemaPayload& Settings,
				FSourceGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
				FDecodedImageImportValue Value;
				std::string Error;
				if (!Root || !TranslateTexture2DSource(
					Root->GetBytes(), Value.SourceData, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ProviderFailure,
						.Identity = "Durin.Image.DecodeFailed",
						.Phase = "Translation", .SourceIdentity = "root",
						.Message = Error.empty() ? "Root image source is unavailable." : Error});
					return false;
				}
				Value.SourcePath = Root->SourcePath;
				Value.SourceHash = Root->ContentHash;
				Value.SourceFileSize = Root->ByteCount;
				Value.SourceLastWriteTime = Root->LastWriteTime;
				return Builder.AddNode({
					.StableIdentity = "image",
					.NodeKind = "Durin.Image.RGBA8",
					.Payload = EncodeImageImportValue(Value),
					.SourceIdentities = {"root"}});
			}
		};

		class FDefaultTexture2DPlanningPass final : public IPlanningPass
		{
		public:
			auto Execute(
				const FSourceGraph& SourceGraph,
				const FBuildGraph*,
				const FSchemaPayload& Settings,
				FBuildGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FTexture2DImportPlan Plan;
				std::string Error;
				if (!SourceGraph.FindNode("image")
					|| !DecodeTexture2DImportPlan(Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidPlan,
						.Identity = "Durin.Texture2D.InvalidPlan",
						.Phase = "PlanningPass", .Message = std::move(Error)});
					return false;
				}
				return Builder.AddNode({
					.StableIdentity = "texture2d",
					.BuilderId = std::string(Texture2DBuilderId),
					.BuilderContractVersion = 1,
					.OutputClassName = "Durin::DTexture2D",
					.Destination = Plan.Destination,
					.Policy = Plan.Policy,
					.Settings = Settings,
					.SourceNodeReferences = {"image"}});
			}
		};

		class FTexture2DBuildProduct final : public IBuildProduct
		{
		public:
			Asset::Build::FTexture2DBuildProduct Product;
			Asset::Build::FTexture2DPublicationContext Publication;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IBuildProduct> override
			{
				return std::make_unique<FTexture2DBuildProduct>(*this);
			}
		};

		class FTexture2DAssetBuilder final : public IAssetBuilder
		{
		public:
			auto BuildDetachedProduct(
				const FBuildNode& AssetBuilderNode,
				const FSourceGraph& SourceGraph,
				IImportProgressReporter*,
				const std::function<bool()>& IsCancellationRequested,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IBuildProduct> override
			{
				const FSourceNode* Image = SourceGraph.FindNode("image");
				FDecodedImageImportValue Source;
				FTexture2DImportPlan Plan;
				std::string Error;
				if (!Image || !DecodeImageImportValue(Image->Payload, Source, Error)
					|| !DecodeTexture2DImportPlan(AssetBuilderNode.Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.Texture2D.PayloadInvalid",
						.Phase = "ProductBuild", .Message = std::move(Error)});
					return {};
				}
				auto Result = std::make_unique<FTexture2DBuildProduct>();
				const Asset::Build::FTexture2DBuildExecutionControl Control{
					.ShouldCancel = IsCancellationRequested};
				if (!Asset::Build::BuildTexture2D({
					.SourceData = std::move(Source.SourceData),
					.SourceContentHashLow = Source.SourceHash.HashLow,
					.SourceContentHashHigh = Source.SourceHash.HashHigh,
					.Settings = {
						.Usage = Plan.Settings.Usage,
						.CompressionQuality = Plan.Settings.CompressionQuality,
						.AlphaMipMode = Plan.Settings.AlphaMipMode,
						.AlphaCoverageThreshold = Plan.Settings.AlphaCoverageThreshold,
						.MaxResolution = Plan.Settings.MaxResolution,
						.bSRGB = Plan.Settings.bSRGB}}, Result->Product, Error, &Control))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.Texture2D.BuildFailed",
						.Phase = "ProductBuild", .Message = std::move(Error)});
					return {};
				}
				Result->Publication = {
					.SourcePath = std::move(Source.SourcePath),
					.DecoderId = "DurinImage",
					.DecoderVersion = 1,
					.SourceFileSize = Source.SourceFileSize,
					.SourceLastWriteTime = Source.SourceLastWriteTime};
				return Result;
			}

			auto MaterializeCandidate(
				const FBuildNode& AssetBuilderNode,
				std::unique_ptr<IBuildProduct> Product,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* TextureProduct = dynamic_cast<FTexture2DBuildProduct*>(Product.get());
				FAssetPath CandidatePath = AssetBuilderNode.Destination;
				if (AssetBuilderNode.Policy != EImportOutputPolicy::Create
					&& !MakeCandidatePath(AssetBuilderNode.Destination, CandidatePath)) return {};
				DTexture2D* Candidate = nullptr;
				if (!TextureProduct || !Asset::CreateAsset(CandidatePath, Candidate)) return {};
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(
					Candidate, AssetBuilderNode.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::PublishTexture2DProduct(*Candidate,
					std::move(TextureProduct->Product), TextureProduct->Publication, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.Texture2D.MaterializationFailed",
						.Phase = "Materialization", .Message = std::move(Error)});
					Result->Abandon();
					return {};
				}
				return Result;
			}

			auto PrepareImportedStateExchange(
				DObject& TargetObject,
				ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DTexture2D>(&TargetObject);
				auto* Candidate = Cast<DTexture2D>(CandidateObject.GetAsset());
				return Target && Candidate
					? std::make_unique<TNoFailExchange<DTexture2D>>(*Target, *Candidate)
					: nullptr;
			}

			auto ApplyProvenance(
				DObject& AssetObject,
				const FImportProvenance& Provenance,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				auto* Texture = Cast<DTexture2D>(&AssetObject);
				std::vector<std::byte> Bytes;
				std::string Error;
				if (!Texture || !SerializeImportProvenance(Provenance, Bytes, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::PublicationFailure,
						.Identity = "Durin.Texture2D.ProvenanceFailed",
						.Phase = "Publication", .Message = std::move(Error)});
					return false;
				}
				Texture->PublishImportProvenance(std::move(Bytes));
				return true;
			}
		};

		}

	auto MakeTexture2DImportRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FTexture2DImportSettings& Settings,
		EImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		if (MountedSource.IsEmpty() || Destination.ToString().empty()
			|| (Settings.Usage != ETextureUsage::Color
				&& Settings.Usage != ETextureUsage::Normal
				&& Settings.Usage != ETextureUsage::DataMask)
			|| (Settings.CompressionQuality != ETextureCompressionQuality::Low
				&& Settings.CompressionQuality != ETextureCompressionQuality::Normal
				&& Settings.CompressionQuality != ETextureCompressionQuality::High)
			|| (Settings.AlphaMipMode != ETextureAlphaMipMode::Average
				&& Settings.AlphaMipMode != ETextureAlphaMipMode::PreserveCoverage)
			|| !std::isfinite(Settings.AlphaCoverageThreshold)
			|| Settings.AlphaCoverageThreshold <= 0.0f
			|| Settings.AlphaCoverageThreshold >= 1.0f)
		{
			OutError = "Texture2D AssetForge request settings are invalid.";
			return false;
		}
		const EImportOutputPolicy Policy = Mode == EImportMode::Import
			|| Mode == EImportMode::Preview
			? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "Texture2D.AssetForge";
		if (Owner.ConflictIdentities.empty())
			Owner.ConflictIdentities.push_back(Destination.ToString());
		OutRequest = {
			.Mode = Mode,
			.RootSource = MountedSource,
			.TranslatorId = std::string(ImageTranslatorId),
			.TranslatorSettings = MakeSchemaPayload(
				std::string(EmptyTranslatorSettingsSchema), 1, {}),
			.PlanningPassStack = {{
				.PlanningPassId = std::string(Texture2DPlanningPassId),
				.ContractVersion = 1,
				.Settings = EncodeTexture2DImportPlan({
					.Destination = Destination,
					.Settings = Settings,
					.Policy = Policy})}},
			.Destination = Destination,
			.Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		OutError.clear();
		return true;
	}

	auto InspectTexture2DImportProvenance(
		const DTexture2D& Texture,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		if (!Texture.GetImportProvenance().empty())
		{
			const std::string_view Hex = Texture.GetImportProvenance();
			if ((Hex.size() & 1) != 0)
			{
				OutError = "Texture2D AssetForge provenance encoding is malformed.";
				return false;
			}
			auto Nibble = [](char Character) -> int32 {
				if (Character >= '0' && Character <= '9') return Character - '0';
				if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
				return -1;
			};
			std::vector<std::byte> Bytes(Hex.size() / 2);
			for (size_t Index = 0; Index < Bytes.size(); ++Index)
			{
				const int32 High = Nibble(Hex[Index * 2]);
				const int32 Low = Nibble(Hex[Index * 2 + 1]);
				if (High < 0 || Low < 0)
				{
					OutError = "Texture2D AssetForge provenance encoding is malformed.";
					return false;
				}
				Bytes[Index] = static_cast<std::byte>((High << 4) | Low);
			}
			return DeserializeImportProvenance(Bytes, OutProvenance, OutError);
		}
		const FTexture2DSourceImportData& Source = Texture.GetSourceImportData();
		FAssetPath Destination;
		if (!Texture.GetPackage() || !Source.HasSource()
			|| (Source.DecoderId != "DurinImage" && Source.DecoderId != ImageTranslatorId)
			|| !FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination,
				&OutError))
		{
			if (OutError.empty())
				OutError = "Texture2D has no compatible AssetForge or legacy image provenance.";
			return false;
		}
		FTexture2DImportSettings Settings{
			.Usage = Texture.GetUsage(),
			.CompressionQuality = Texture.GetCompressionQuality(),
			.AlphaMipMode = Texture.GetAlphaMipMode(),
			.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
			.MaxResolution = Texture.GetMaxResolution(),
			.bSRGB = Texture.IsSRGB()};
		OutProvenance = {
			.Translator = {
				.Id = std::string(ImageTranslatorId), .ContractVersion = 1,
				.Settings = MakeSchemaPayload(
					std::string(EmptyTranslatorSettingsSchema), 1, {})},
			.PlanningPassStack = {{
				.PlanningPassId = std::string(Texture2DPlanningPassId), .ContractVersion = 1,
				.Settings = EncodeTexture2DImportPlan({
					.Destination = Destination, .Settings = Settings,
					.Policy = EImportOutputPolicy::ReplaceWholeState})}},
			.Sources = {{
				.StableIdentity = "root", .Role = "Root",
				.SourcePath = Source.Source.SourcePath,
				.ContentHash = {.HashLow = Source.Source.SourceContentHashLow,
					.HashHigh = Source.Source.SourceContentHashHigh},
				.ByteCount = Texture.GetSourceFileSize()}},
			.OutputMappings = {{
				.SourceNodeIdentity = "image", .OutputIdentity = "texture2d",
				.AssetPath = Destination}},
			.AuthoredOutputFingerprint = Texture.GetDerivedDataKey()};
		OutError.clear();
		return true;
	}

	auto RegisterTexture2DImportProvider(FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& Registrations,
		std::string& OutError) -> bool
	{
		auto Add = [&](FComponentRegistration Value) {
			if (!Value) return false;
			Registrations.push_back(std::move(Value));
			return true;
		};
		return Add(Service.RegisterSourceTranslatorScoped({.Descriptor = {
				.Identity = {.Id = std::string(ImageTranslatorId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(EmptyTranslatorSettingsSchema), .SchemaVersion = 1}},
				.Extensions = {".png", ".jpg", ".jpeg", ".bmp", ".tga"}, .Priority = 100,
				.TranslationThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FImageSourceTranslator>()}, OwnerGate, OutError))
			&& Add(Service.RegisterPlanningPassScoped({.Descriptor = {
				.Identity = {.Id = std::string(Texture2DPlanningPassId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(Texture2DPlanSchema), .SchemaVersion = 1}},
				.Priority = 100, .ExecutionThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FDefaultTexture2DPlanningPass>()}, OwnerGate, OutError))
			&& Add(Service.RegisterAssetBuilderScoped({.Descriptor = {
				.Identity = {.Id = std::string(Texture2DBuilderId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(Texture2DPlanSchema), .SchemaVersion = 1}},
				.OutputClassName = "Durin::DTexture2D", .Priority = 100,
				.ProductBuildThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTexture2DAssetBuilder>()}, OwnerGate, OutError));
	}}
