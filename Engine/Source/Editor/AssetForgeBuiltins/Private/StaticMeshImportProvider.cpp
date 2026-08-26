#include "StaticMeshImportProviderSchema.h"
#include "BuiltinImportProviderCommon.h"
#include "BuiltinImportProvenance.h"
#include "DObject/Package.h"
#include "BuiltinProviderRegistration.h"
#include "Asset/AssetAuthoringOperations.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	class FStaticMeshExchange final : public IPreparedImportedStateExchange
		{
		public:
			explicit FStaticMeshExchange(std::unique_ptr<FStaticMeshImportedStateExchange> InExchange)
				: Exchange(std::move(InExchange)) {}
			auto Commit() noexcept -> void override { Exchange->Commit(); }
			auto Reverse() noexcept -> void override { Exchange->Reverse(); }
			auto Finalize() noexcept -> void override { Exchange->Finalize(); }
		private:
			std::unique_ptr<FStaticMeshImportedStateExchange> Exchange;
		};

		class FGeometrySourceTranslator final : public ISourceTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				std::string Extension(Source.Extension);
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Value) {
					return static_cast<char>(std::tolower(Value));
				});
				return Extension == ".obj" || Extension == ".fbx"
					|| Extension == ".gltf" || Extension == ".glb"
					|| Extension == ".dae" || Extension == ".3ds"
					|| Extension == ".ply" || Extension == ".stl";
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
				FImportedSceneData Scene;
				FStaticMeshImportSettings ImportSettings;
				std::string Error;
				if (!Root || !DecodeGeometryTranslatorSettings(
					Settings, ImportSettings, Error)
					|| !ImportGeometryFromMemory(
					Root->GetBytes(),
					std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
					Scene, MakeMeshImportOptions(ImportSettings,
						Root->SourcePath)))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ProviderFailure,
						.Identity = "Durin.Geometry.DecodeFailed",
						.Phase = "Translation", .SourceIdentity = "root",
						.Message = Error.empty()
							? "Geometry source could not be decoded." : std::move(Error)});
					return false;
				}
				FDecodedStaticMeshImportValue Value{
					.ImportedData = MakeStaticMeshImportedData(Scene),
					.SourcePath = Root->SourcePath,
					.SourceHash = Root->ContentHash};
				return Builder.AddNode({
					.StableIdentity = "mesh:combined",
					.NodeKind = "Durin.Geometry.StaticMesh",
					.Payload = EncodeStaticMeshImportValue(Value),
					.SourceIdentities = {"root"}});
			}
		};

		class FDefaultStaticMeshPlanningPass final : public IPlanningPass
		{
		public:
			auto Execute(
				const FSourceGraph& SourceGraph,
				const FBuildGraph*,
				const FSchemaPayload& Settings,
				FBuildGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FStaticMeshImportPlan Plan;
				std::string Error;
				if (!SourceGraph.FindNode("mesh:combined")
					|| !DecodeStaticMeshImportPlan(Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidPlan,
						.Identity = "Durin.StaticMesh.InvalidPlan",
						.Phase = "PlanningPass", .Message = std::move(Error)});
					return false;
				}
				return Builder.AddNode({
					.StableIdentity = "static-mesh",
					.BuilderId = std::string(StaticMeshBuilderId),
					.BuilderContractVersion = 1,
					.OutputClassName = "Durin::DStaticMesh",
					.Destination = Plan.Destination,
					.Policy = Plan.Policy,
					.Settings = Settings,
					.SourceNodeReferences = {"mesh:combined"}});
			}
		};

		auto CloneStaticMeshBuildProduct(
			const Asset::FStaticMeshBuildProduct& Source)
			-> Asset::FStaticMeshBuildProduct
		{
			Asset::FStaticMeshBuildProduct Result;
			if (Source.RenderData)
				Result.RenderData = std::make_unique<FStaticMeshRenderData>(*Source.RenderData);
			Result.MaterialSlots = Source.MaterialSlots;
			Result.SourceImportData = Source.SourceImportData;
			Result.NormalizedSize = Source.NormalizedSize;
			Result.DerivedDataKey = Source.DerivedDataKey;
			Result.bSlotMetadataChanged = Source.bSlotMetadataChanged;
			Result.DerivedDataStatus = Source.DerivedDataStatus;
			Result.DiagnosticMessage = Source.DiagnosticMessage;
			Result.bSourceImporterInvoked = Source.bSourceImporterInvoked;
			Result.bMarkPackageDirty = Source.bMarkPackageDirty;
			Result.FailureStage = Source.FailureStage;
			return Result;
		}

		class FStaticMeshBuildProduct final : public IBuildProduct
		{
		public:
			Asset::FStaticMeshBuildProduct Product;
			Asset::FStaticMeshImportedData ImportedData;
			FStaticMeshSourceImportData SourceImportData;
			std::string SourceLabel;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IBuildProduct> override
			{
				auto Result = std::make_unique<FStaticMeshBuildProduct>();
				Result->Product = CloneStaticMeshBuildProduct(Product);
				Result->ImportedData = ImportedData;
				Result->SourceImportData = SourceImportData;
				Result->SourceLabel = SourceLabel;
				return Result;
			}
		};

		class FStaticMeshReconciliationContext final
			: public IReconciliationContext
		{
		public:
			Asset::FStaticMeshReconciliationSnapshot Snapshot;
		};

		class FStaticMeshAssetBuilder final : public IAssetBuilder
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
				if (IsCancellationRequested()) return {};
				const FSourceNode* MeshNode =
					SourceGraph.FindNode("mesh:combined");
				FDecodedStaticMeshImportValue Source;
				FStaticMeshImportPlan Plan;
				std::string Error;
				if (!MeshNode
					|| !DecodeStaticMeshImportValue(MeshNode->Payload, Source, Error)
					|| !DecodeStaticMeshImportPlan(AssetBuilderNode.Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.StaticMesh.PayloadInvalid",
						.Phase = "ProductBuild", .Message = std::move(Error)});
					return {};
				}
				FStaticMeshSourceImportData Provenance{
					.SourcePath = Source.SourcePath,
					.SourceContentHash = Source.SourceHash.ToString(),
					.ImporterId = std::string(StaticMeshImporterId),
					.ImporterVersion = StaticMeshAssimpImporterVersion,
					.ImportSettings = Plan.Settings};
				Asset::FStaticMeshReconciliationSnapshot Reconciliation{
					.StableObjectPath = Plan.Destination.ToString(),
					.Provenance = Provenance,
					.ImportSettings = Plan.Settings};
				auto Result = std::make_unique<FStaticMeshBuildProduct>();
				Result->ImportedData = Source.ImportedData;
				Result->SourceImportData = Provenance;
				Result->SourceLabel = Source.SourcePath.Path;
				if (AssetBuilderNode.Policy == EImportOutputPolicy::Create
					&& !Asset::FStaticMeshBuildOperations::BuildImportedProduct(
					Reconciliation, Result->ImportedData, Provenance,
					Source.SourcePath.Path, Result->Product, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.StaticMesh.BuildFailed",
						.Phase = "ProductBuild", .Message = std::move(Error)});
					return {};
				}
				return Result;
			}

			auto CaptureReconciliationContext(
				const FBuildNode&,
				const DObject& ExistingTarget,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IReconciliationContext> override
			{
				const auto* Mesh = Cast<DStaticMesh>(&ExistingTarget);
				if (!Mesh) return {};
				auto Result = std::make_unique<FStaticMeshReconciliationContext>();
				Result->Snapshot =
					Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(*Mesh);
				return Result;
			}

			auto ReconcileDetachedProduct(
				const FBuildNode& AssetBuilderNode,
				const IReconciliationContext* Context,
				IBuildProduct& Product,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				if (AssetBuilderNode.Policy == EImportOutputPolicy::Create) return true;
				const auto* Reconciliation =
					dynamic_cast<const FStaticMeshReconciliationContext*>(Context);
				auto* MeshProduct = dynamic_cast<FStaticMeshBuildProduct*>(&Product);
				std::string Error;
				if (!Reconciliation || !MeshProduct
					|| !Asset::FStaticMeshBuildOperations::BuildImportedProduct(
						Reconciliation->Snapshot,
						MeshProduct->ImportedData, MeshProduct->SourceImportData,
						MeshProduct->SourceLabel, MeshProduct->Product, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.StaticMesh.ReconciliationFailed",
						.Phase = "Reconciliation", .Message = std::move(Error)});
					return false;
				}
				MeshProduct->Product.DiagnosticMessage =
					"Rebuilt static mesh after cache miss or source change.";
				return true;
			}

			auto MaterializeCandidate(
				const FBuildNode& AssetBuilderNode,
				std::unique_ptr<IBuildProduct> Product,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* MeshProduct = dynamic_cast<FStaticMeshBuildProduct*>(Product.get());
				FAssetPath CandidatePath = AssetBuilderNode.Destination;
				if (AssetBuilderNode.Policy != EImportOutputPolicy::Create
					&& !MakeCandidatePath(AssetBuilderNode.Destination, CandidatePath)) return {};
				DStaticMesh* Candidate = nullptr;
				if (!MeshProduct || !Asset::CreateAsset(CandidatePath, Candidate)) return {};
				auto Result = std::make_unique<FBuiltinSingleAssetCandidate>(
					Candidate, AssetBuilderNode.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::FStaticMeshBuildOperations::PublishImportedProduct(
					*Candidate, std::move(MeshProduct->Product), Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.StaticMesh.MaterializationFailed",
						.Phase = "Materialization", .Message = std::move(Error)});
					Result->Abandon();
					return {};
				}
				return Result;
			}

			auto PrepareImportedStateExchange(
				DObject& TargetObject,
				ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DStaticMesh>(&TargetObject);
				auto* Candidate = Cast<DStaticMesh>(CandidateObject.GetAsset());
				std::string Error;
				auto Exchange = Target && Candidate
					? Target->PrepareImportedStateExchange(*Candidate, Error) : nullptr;
				if (!Exchange)
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.StaticMesh.ExchangeFailed",
						.Phase = "Exchange", .Message = std::move(Error)});
				return Exchange
					? std::make_unique<FStaticMeshExchange>(std::move(Exchange)) : nullptr;
			}

			auto HasAuthoredRecoveryChanges(
				const DObject& TargetObject,
				const ISingleAssetCandidate& CandidateObject) const -> bool override
			{
				const auto* Target = Cast<DStaticMesh>(&TargetObject);
				const auto* Previous = Cast<DStaticMesh>(CandidateObject.GetAsset());
				return !Target || !Previous
					|| Target->GetSourceImportData() != Previous->GetSourceImportData()
					|| !std::ranges::equal(
						Target->GetMaterialSlots(), Previous->GetMaterialSlots());
			}

			auto ApplyProvenance(
				DObject& AssetObject,
				const FImportProvenance& Provenance,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				std::vector<std::byte> Bytes;
				std::string Error;
				if (!Mesh || !SerializeImportProvenance(Provenance, Bytes, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::PublicationFailure,
						.Identity = "Durin.StaticMesh.ProvenanceFailed",
						.Phase = "Publication", .Message = std::move(Error)});
					return false;
				}
				Mesh->PublishImportProvenance(std::move(Bytes));
				return true;
			}
		};

		}

	auto InspectStaticMeshSource(
			const DStaticMesh& Mesh) -> FStaticMeshSourceDiagnostic
		{
			const FStaticMeshSourceImportData& Source = Mesh.GetSourceImportData();
			if (!Source.HasSource()) return {};
			FMountedSourceResolution Resolution;
			std::string Error;
			if (!Mesh.GetPackage())
				return {EStaticMeshSourceStatus::Invalid, {},
					"Static mesh source cannot be resolved without an owning package."};
			if (!ResolveMountedSourceReference(
				Mesh.GetPackage()->GetPackagePath(), Source.SourcePath.Path,
				EMountedSourceExistencePolicy::AllowMissing, Resolution, Error))
				return {EStaticMeshSourceStatus::Invalid, {}, std::move(Error)};
			if (!Resolution.bExists)
			{
				return {
					EStaticMeshSourceStatus::Missing,
					Resolution.PhysicalPath.generic_string(),
					std::format(
						"Static mesh source is missing: {}. Use source-path repair to select its replacement.",
						Source.SourcePath.Path)};
			}
			std::string CurrentHash;
			if (!HashStaticMeshSource(Resolution.PhysicalPath, CurrentHash, Error))
				return {
					EStaticMeshSourceStatus::Invalid,
					Resolution.PhysicalPath.generic_string(),
					std::move(Error)};
			if (!Source.SourceContentHash.empty()
				&& CurrentHash != Source.SourceContentHash)
			{
				return {
					EStaticMeshSourceStatus::Changed,
					Resolution.PhysicalPath.generic_string(),
					"The mounted static-mesh source bytes changed since this asset was last imported."};
			}
			return {EStaticMeshSourceStatus::Available,
				Resolution.PhysicalPath.generic_string(), {}};
		}

		auto ChangeStaticMeshSourceReference(
			DStaticMesh& Mesh,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool
		{
			if (!Mesh.GetPackage())
			{
				OutError = "Only packaged static meshes can retain source provenance.";
				return false;
			}
			FMountedSourceResolution Source;
			if (!ResolveMountedSourceReference(
				Mesh.GetPackage()->GetPackagePath(), SourceVirtualPath,
				EMountedSourceExistencePolicy::RequireFile, Source, OutError))
				return false;
			std::optional<FImportProvenance> Existing;
			if (Mesh.GetSourceImportData().HasSource())
			{
				FImportProvenance Provenance;
				if (!InspectStaticMeshImportProvenance(Mesh, Provenance, OutError))
					return false;
				Existing = std::move(Provenance);
			}
			FAssetPath Destination;
			if (!FAssetPath::TryCreate(
				Mesh.GetPackage()->GetPackagePath(), Destination, &OutError)) return false;
			FImportRequest Request;
			if (!MakeStaticMeshImportRequest(
				Source.SourcePath, Destination, Mesh.GetImportSettings(),
				EImportMode::ReplaceSource,
				{.OwnerId = std::format("StaticMesh.ReplaceSource:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Existing), Request, OutError)) return false;
			const FImportResult Result = GetImportService().RunImportInline(
				std::move(Request),
				std::format("Replace StaticMesh source {}", Destination.GetAssetName()));
			if (Result.Outcome.State != EImportOperationState::Succeeded)
			{
				OutError = Result.Outcome.Diagnostic.empty()
					? "StaticMesh AssetForge source replacement failed."
					: Result.Outcome.Diagnostic;
				return false;
			}
			OutError.clear();
			return true;
		}

		auto IngestAndChangeStaticMeshSource(
			DStaticMesh& Mesh,
			std::string_view FilePath,
			std::string_view TargetSourceVirtualPath,
			std::string& OutError) -> bool
		{
			if (!Mesh.GetPackage())
			{
				OutError = "Only packaged static meshes can retain source provenance.";
				return false;
			}
			FScopedMountedSourceFile Source;
			if (!PrepareMountedSourceFile(
				FilePath, Mesh.GetPackage()->GetPackagePath(),
				TargetSourceVirtualPath, Source, OutError)) return false;
			const bool bChanged = ChangeStaticMeshSourceReference(
				Mesh, Source.SourcePath.Path, OutError);
			if (bChanged) Source.Commit();
			return bChanged;
		}

		auto CreateTransientStaticMeshFromFile(
			std::string_view FilePath,
			DObject* Outer,
			std::string_view ObjectName,
			std::string& OutError,
			const FStaticMeshImportSettings& ImportSettings) -> DStaticMesh*
		{
			const std::filesystem::path Input =
				std::filesystem::absolute(FilePath).lexically_normal();
			if (!std::filesystem::is_regular_file(Input))
			{
				OutError = std::format(
					"Static mesh source file does not exist: {}", Input.generic_string());
				return nullptr;
			}
			if (!ImportSettings.IsValid(&OutError)) return nullptr;

			DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, ObjectName);
			std::string SourceHash;
			FStaticMeshAuthoringProduct Product;
			if (HashStaticMeshSource(Input, SourceHash, OutError)
				&& BuildStaticMeshFileProduct(
					*Mesh, Input.generic_string(),
					{
						.SourcePath = {.Path = Input.generic_string()},
						.SourceContentHash = std::move(SourceHash),
						.ImporterId = std::string(StaticMeshImporterId),
						.ImporterVersion = StaticMeshAssimpImporterVersion,
						.ImportSettings = ImportSettings},
					Input.generic_string(), Product, OutError)
				&& Mesh->PublishImportedProduct(std::move(Product), OutError)) return Mesh;
			MarkAsGarbage(Mesh);
			return nullptr;
		}

		auto ImportStaticMeshAsset(
			std::string_view FilePath,
			std::string_view AssetPath,
			const FStaticMeshImportSettings& ImportSettings,
			std::string_view SourceDestination,
			bool bEngineAuthoringContext) -> FStaticMeshImportResult
		{
			const std::filesystem::path Input =
				std::filesystem::absolute(FilePath).lexically_normal();
			if (!std::filesystem::is_regular_file(Input))
				return {false, "Source file does not exist.", nullptr};
			std::string Error;
			if (!ImportSettings.IsValid(&Error)) return {false, std::move(Error), nullptr};

			FAssetPath ParsedAssetPath;
			if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
				return {false, std::move(Error), nullptr};
			if (Asset::FindAssetExact(ParsedAssetPath)
				|| Asset::FindResidentPackage(ParsedAssetPath))
				return {
					false,
					std::format("Asset {} already exists.", ParsedAssetPath.ToString()),
					nullptr};

			std::filesystem::path Destination;
			std::string StoredSourcePath;
			if (!MakeCanonicalStaticMeshSourceLocation(
				ParsedAssetPath, Input.extension().generic_string(), SourceDestination,
				Destination, StoredSourcePath, Error))
				return {false, std::move(Error), nullptr};
			FScopedMountedSourceFile MountedSource;
			if (!PrepareMountedSourceFile(
				Input, ParsedAssetPath.ToString(), StoredSourcePath, MountedSource, Error,
				bEngineAuthoringContext
					? EMountedSourceMutationContext::EngineAuthoring
					: EMountedSourceMutationContext::DependencySafe))
				return {false, std::move(Error), nullptr};
			FImportRequest Request;
			if (!MakeStaticMeshImportRequest(
				MountedSource.SourcePath, ParsedAssetPath, ImportSettings,
				EImportMode::Import,
				{.OwnerId = std::format("StaticMesh.Import:{}", ParsedAssetPath.ToString()),
					.ConflictIdentities = {ParsedAssetPath.ToString()}},
				{}, Request, Error))
				return {false, std::move(Error), nullptr};
			const FImportResult Imported = GetImportService().RunImportInline(
				std::move(Request),
				std::format("Import StaticMesh {}", ParsedAssetPath.GetAssetName()));
			if (Imported.Outcome.State != EImportOperationState::Succeeded)
				return {false, Imported.Outcome.Diagnostic.empty()
					? "StaticMesh AssetForge import failed."
					: Imported.Outcome.Diagnostic, nullptr};
			DObject* ImportedObject = nullptr;
			(void)Asset::LoadAsset(ParsedAssetPath, ImportedObject);
			auto* Mesh = Cast<DStaticMesh>(ImportedObject);
			if (!Mesh)
				return {false, "StaticMesh AssetForge import published no destination asset.", nullptr};
			MountedSource.Commit();
			return {true, {}, Mesh};
		}
	auto MakeStaticMeshImportRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FStaticMeshImportSettings& Settings,
		EImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		if (MountedSource.IsEmpty() || !Destination.IsValid()
			|| !Settings.IsValid(&OutError))
		{
			if (OutError.empty()) OutError = "StaticMesh AssetForge request is invalid.";
			return false;
		}
		const EImportOutputPolicy Policy = Mode == EImportMode::Import
			|| Mode == EImportMode::Preview
			? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "StaticMesh.AssetForge";
		if (Owner.ConflictIdentities.empty())
			Owner.ConflictIdentities.push_back(Destination.ToString());
		std::optional<FImportProvenance> PersistedProvenance = std::move(ExistingProvenance);
		std::string ProvenanceError;
		if (PersistedProvenance && !PersistedProvenance->Validate(ProvenanceError))
			PersistedProvenance.reset();
		OutRequest = {
			.Mode = Mode,
			.RootSource = MountedSource,
			.TranslatorId = std::string(GeometryTranslatorId),
			.TranslatorSettings = EncodeGeometryTranslatorSettings(Settings),
			.PlanningPassStack = {{
				.PlanningPassId = std::string(StaticMeshPlanningPassId),
				.ContractVersion = 1,
				.Settings = EncodeStaticMeshImportPlan({
					.Destination = Destination, .Settings = Settings, .Policy = Policy})}},
			.Destination = Destination,
			.Owner = std::move(Owner),
			.ExistingProvenance = std::move(PersistedProvenance)};
		OutError.clear();
		return true;
	}

	auto SubmitStaticMeshImport(std::string_view FilePath,
		const FAssetPath& Destination, const FStaticMeshImportSettings& Settings,
		std::string_view SourceDestination, bool bEngineAuthoringContext,
		FImportCompletion Completion, std::string& OutError)
		-> FImportHandle
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input) || !Settings.IsValid(&OutError))
		{
			if (OutError.empty()) OutError = "StaticMesh source is unavailable.";
			return {};
		}
		std::filesystem::path PhysicalDestination;
		std::string StoredSourcePath;
		if (!MakeCanonicalStaticMeshSourceLocation(Destination,
			Input.extension().generic_string(), SourceDestination,
			PhysicalDestination, StoredSourcePath, OutError)) return {};
		auto Mounted = std::make_shared<FScopedMountedSourceFile>();
		if (!PrepareMountedSourceFile(Input, Destination.ToString(), StoredSourcePath,
			*Mounted, OutError, bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe)) return {};
		FImportRequest Request;
		if (!MakeStaticMeshImportRequest(Mounted->SourcePath, Destination, Settings,
			EImportMode::Import,
			{.OwnerId = std::format("StaticMesh.Import:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}}, {}, Request, OutError)) return {};
		OutError.clear();
		return GetImportService().SubmitImport(std::move(Request),
			std::format("Import StaticMesh {}", Destination.GetAssetName()),
			[Mounted, Completion = std::move(Completion)](const FImportResult& Result) {
				if (Result.Outcome.State == EImportOperationState::Succeeded) Mounted->Commit();
				if (Completion) Completion(Result);
			});
	}

	auto InspectStaticMeshImportProvenance(
		const DStaticMesh& Mesh,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		if (Mesh.GetImportProvenance().empty())
		{
			OutError = "StaticMesh has no current AssetForge provenance.";
			return false;
		}
		return DecodeStoredImportProvenance(
			Mesh.GetImportProvenance(), OutProvenance, OutError);
	}

	auto RegisterStaticMeshImportProvider(FImportService& Service,
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
				.Identity = {.Id = std::string(GeometryTranslatorId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(GeometryTranslatorSettingsSchema), .SchemaVersion = 1}},
				.Extensions = {".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"},
				.Priority = 100, .TranslationThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FGeometrySourceTranslator>()}, OwnerGate, OutError))
			&& Add(Service.RegisterPlanningPassScoped({.Descriptor = {
				.Identity = {.Id = std::string(StaticMeshPlanningPassId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(StaticMeshPlanSchema), .SchemaVersion = 1}},
				.Priority = 100, .ExecutionThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FDefaultStaticMeshPlanningPass>()}, OwnerGate, OutError))
			&& Add(Service.RegisterAssetBuilderScoped({.Descriptor = {
				.Identity = {.Id = std::string(StaticMeshBuilderId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(StaticMeshPlanSchema), .SchemaVersion = 1}},
				.OutputClassName = "Durin::DStaticMesh", .Priority = 100,
				.ProductBuildThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FStaticMeshAssetBuilder>()}, OwnerGate, OutError));
	}}
