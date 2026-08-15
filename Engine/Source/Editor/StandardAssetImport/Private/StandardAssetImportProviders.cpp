#include "StandardAssetImportProviders.h"
#include "StandardAssetAuthoringFeatures.h"
#include "StaticMeshSourceTranslation.h"
#include "Texture2DSourceTranslation.h"
#include "Texture2DBuildAdapter.h"
#include "TextureCubeSourceTranslation.h"
#include "Texture2DPropertyEditing.h"
#include "Texture2DPostLoad.h"
#include "Texture2DSourceRelocation.h"
#include "TextureCubePostLoadPolicy.h"
#include "TerrainHeightmapSourceTranslation.h"

#include "Animation/AnimationClip.h"
#include "ImportedScene.h"
#include "AssetImportCore.h"
#include "AssetMutation.h"
#include "DObject/ObjectLifecycle.h"
#include "EncodedSourceSnapshot.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SceneImport.h"
#include "SceneImportInternal.h"
#include "AsyncImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "Asset/MountedSource.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubeBuilder.h"
#include "TextureCubeBuildAdapter.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "TerrainHeightmapBuildAdapter.h"

namespace Durin::Asset::Import::Standard
{
	namespace
	{

		inline constexpr uint32 StaticMeshAssimpImporterVersion = 3;
		inline constexpr std::string_view StaticMeshImporterId = "Assimp";
		inline constexpr std::string_view StaticMeshSourceRoot = "Models";

		auto FindOwningMount(std::string_view AssetPath) -> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalStaticMeshSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::filesystem::path& OutPhysicalPath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format(
					"Static mesh asset {} is not beneath a registered package mount.",
					AssetPath.ToString());
				return false;
			}
			if (RequestedSourcePath.empty())
			{
				std::filesystem::path RelativeAssetPath(
					std::string(AssetPath.ToString().substr(Mount->VirtualRoot.size())));
				RelativeAssetPath.replace_extension(Extension);
				const std::filesystem::path StoredPath =
					std::filesystem::path(StaticMeshSourceRoot) / RelativeAssetPath;
				OutStoredPath = Mount->VirtualRoot
					+ StoredPath.lexically_normal().generic_string();
			}
			else
			{
				OutStoredPath = RequestedSourcePath;
			}
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					OutStoredPath, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPhysicalPath = Resolved.PhysicalPath;
			return true;
		}

		auto ResolvePortableStaticMeshSource(
			const DStaticMesh& Mesh,
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const FStaticMeshSourceImportData& Source = Mesh.GetSourceImportData();
			if (!Mesh.GetPackage())
			{
				OutError = "Static mesh source cannot be resolved without an owning package.";
				return false;
			}
			const PathUtilities::FMountPolicyResult Dependency =
				PathUtilities::CheckMountDependency(
					Mesh.GetPackage()->GetPackagePath(), Source.SourcePath.Path);
			if (!Dependency)
			{
				OutError = Dependency.Message;
				return false;
			}
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					Source.SourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPath = Resolved.PhysicalPath;
			return true;
		}

		auto HashStaticMeshSource(
			const std::filesystem::path& Path,
			std::string& OutHash,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutError = std::format(
					"Failed to read static mesh source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes).ToString();
			return true;
		}

		template<typename T>
		auto AppendValue(std::vector<uint8>& Bytes, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* Begin = reinterpret_cast<const uint8*>(&Value);
			Bytes.insert(Bytes.end(), Begin, Begin + sizeof(T));
		}

		auto MakePayload(std::string SchemaId, uint32 Version, std::vector<uint8> Bytes)
			-> FImportPayload
		{
			FImportPayload Payload{
				.SchemaId = std::move(SchemaId),
				.SchemaVersion = Version,
				.Bytes = std::move(Bytes)};
			std::string Error;
			requiref(Payload.Finalize(Error), "{}", Error);
			return Payload;
		}

		auto MakeStaticMeshSettings(const FStaticMeshImportSettings& Settings) -> FImportPayload
		{
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Settings.ForwardAxis);
			AppendValue(Bytes, Settings.RightAxis);
			AppendValue(Bytes, Settings.UpAxis);
			return MakePayload("Durin.StaticMesh.ImportSettings", 1, std::move(Bytes));
		}

		auto ImportAxisVector(EStaticMeshImportAxis Axis) -> FVector3f
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: return {1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveY: return {0.0f, 1.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeY: return {0.0f, -1.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveZ: return {0.0f, 0.0f, 1.0f};
			case EStaticMeshImportAxis::NegativeZ: return {0.0f, 0.0f, -1.0f};
			}
			return {};
		}

		auto MakeMeshImportOptions(
			const FStaticMeshImportSettings& Settings,
			const FSourcePath& RootSource) -> FMeshImportOptions
		{
			const FVector3f Forward = ImportAxisVector(Settings.ForwardAxis);
			const FVector3f Right = ImportAxisVector(Settings.RightAxis);
			const FVector3f Up = ImportAxisVector(Settings.UpAxis);
			FMeshImportOptions Options;
			for (uint32 Component = 0; Component < 3; ++Component)
			{
				Options.SourceToEngine[Component][0] = Forward[Component];
				Options.SourceToEngine[Component][1] = Right[Component];
				Options.SourceToEngine[Component][2] = Up[Component];
			}
			Options.RootSource = RootSource;
			return Options;
		}

		auto DecodeStaticMeshSource(
			std::string_view FilePath,
			const FStaticMeshImportSettings& Settings,
			Asset::Build::FStaticMeshImportedData& OutData,
			std::string& OutError) -> bool
		{
			FImportedSceneData Scene;
			if (ImportFromFile(
				FilePath, Scene, MakeMeshImportOptions(Settings, {})))
			{
				OutData = MakeStaticMeshImportedData(Scene);
				OutError.clear();
				return true;
			}
			OutError = std::format("Failed to decode StaticMesh source {}.", FilePath);
			return false;
		}

			auto BuildStaticMeshFileProduct(
			DStaticMesh& Mesh,
			std::string_view FilePath,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceLabel,
			FStaticMeshAuthoringProduct& OutProduct,
			std::string& OutError) -> bool
		{
			Asset::Build::FStaticMeshImportedData ImportedData;
			if (!DecodeStaticMeshSource(
				FilePath, SourceImportData.ImportSettings, ImportedData, OutError))
				return false;
			return Asset::Build::FStaticMeshBuildOperations::BuildImportedProduct(
				Asset::Build::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
				ImportedData, std::move(SourceImportData), SourceLabel,
				OutProduct, OutError);
		}

		auto PostLoadStaticMesh(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool
		{
			const FStaticMeshSourceDiagnostic SourceDiagnostic =
				InspectStaticMeshSource(Mesh);
			if (SourceDiagnostic.Status == EStaticMeshSourceStatus::NoSource)
			{
				OutDiagnostic = {};
				OutError.clear();
				return true;
			}
			if (Mesh.GetMaterialSlots().empty())
			{
				OutError = "StaticMesh with source metadata must contain a material slot.";
				return false;
			}

			FStaticMeshSourceImportData Source = Mesh.GetSourceImportData();
			const bool bSourceAvailable = SourceDiagnostic.IsAvailable();
			if (bSourceAvailable)
			{
				std::vector<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, SourceDiagnostic.ResolvedPath))
				{
					OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
					OutError = std::format(
						"Failed to read StaticMesh source file: {}",
						SourceDiagnostic.ResolvedPath);
					OutDiagnostic.Message = OutError;
					return false;
				}
				Source.SourceContentHash = FXxHash128::HashBuffer(Bytes).ToString();
			}
			const bool bSourceHashValid = Source.SourceContentHash.size() == 32
				&& std::ranges::all_of(Source.SourceContentHash, [](char Character) {
					return Character >= '0' && Character <= '9'
						|| Character >= 'a' && Character <= 'f';
				});
			if (!bSourceHashValid)
			{
				OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
				OutError = SourceDiagnostic.Message.empty()
					? "StaticMesh source hash is unavailable."
					: SourceDiagnostic.Message;
				OutDiagnostic.Message = OutError;
				return false;
			}

			const bool bSourceMetadataStale = bSourceAvailable
				&& Mesh.GetSourceImportData().SourceContentHash
					!= Source.SourceContentHash;
			FStaticMeshAuthoringProduct Product;
			EStaticMeshDerivedDataStatus CacheStatus =
				EStaticMeshDerivedDataStatus::Missing;
			std::string CacheMessage;
			if (!bSourceMetadataStale
				&& Asset::Build::FStaticMeshBuildOperations::LoadDerivedDataProduct(
					Asset::Build::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
					Source, bSourceAvailable, Product, CacheStatus,
					CacheMessage, OutError))
			{
				return Mesh.PublishImportedProduct(std::move(Product), OutError);
			}
			if (!bSourceAvailable)
			{
				OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
				OutDiagnostic.Message = std::format(
					"{}. Cached payload was unavailable: {} Reimport and cache regeneration are unavailable.",
					SourceDiagnostic.Message, CacheMessage);
				OutError = OutDiagnostic.Message;
				return false;
			}

			if (!BuildStaticMeshFileProduct(
				Mesh, SourceDiagnostic.ResolvedPath, std::move(Source),
				SourceDiagnostic.ResolvedPath, Product, OutError))
			{
				OutDiagnostic.Status = Product.FailureStage
					== EStaticMeshAuthoringFailureStage::DerivedDataWrite
					? EStaticMeshDerivedDataStatus::WriteFailure
					: CacheStatus;
				OutDiagnostic.Message = OutError;
				OutDiagnostic.bSourceImporterInvoked = true;
				return false;
			}
			Product.DerivedDataStatus = EStaticMeshDerivedDataStatus::Rebuilt;
			Product.DiagnosticMessage = std::format(
				"Rebuilt static mesh after cache miss: {}", CacheMessage);
			return Mesh.PublishImportedProduct(std::move(Product), OutError);
		}


		auto MakeTexture2DSettings(const DTexture2D& Texture) -> FImportPayload
		{
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Texture.GetUsage());
			AppendValue(Bytes, Texture.GetCompressionQuality());
			AppendValue(Bytes, Texture.GetAlphaMipMode());
			AppendValue(Bytes, Texture.GetAlphaCoverageThreshold());
			AppendValue(Bytes, Texture.GetMaxResolution());
			const bool bSRGB = Texture.IsSRGB();
			AppendValue(Bytes, bSRGB);
			return MakePayload("Durin.Texture2D.ImportSettings", 1, std::move(Bytes));
		}

		auto MakeTextureCubeSettings(const DTextureCube& Texture) -> FImportPayload
		{
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Texture.GetSourceLayout());
			AppendValue(Bytes, Texture.GetPanoramaFaceDimension());
			AppendValue(Bytes, Texture.GetPanoramaExposureEV());
			const bool bSRGB = Texture.IsSRGB();
			AppendValue(Bytes, bSRGB);
			return MakePayload("Durin.TextureCube.ImportSettings", 1, std::move(Bytes));
		}

		auto MakeTerrainHeightmapSettings() -> FImportPayload
		{
			return MakePayload("Durin.TerrainHeightmap.ImportSettings", 1, {});
		}

		auto MakeSourceHash(const FTextureSourceFile& Source) -> FXxHash128
		{
			FXxHash128 Hash;
			Hash.HashLow = Source.SourceContentHashLow;
			Hash.HashHigh = Source.SourceContentHashHigh;
			return Hash;
		}

		auto MakeCandidatePath(const FAssetPath& TargetPath, FAssetPath& OutPath) -> bool
		{
			for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
			{
				if (!FAssetPath::TryCreate(std::format("{}_ImportCandidate_{}",
					TargetPath.ToString(), Suffix), OutPath)) return false;
				if (!Asset::FindLoadedPackage(OutPath)
					&& !Asset::FindDraftPackage(OutPath)
					&& !Asset::FindAssetExact(OutPath)) return true;
			}
			return false;
		}

		class FEngineSingleAssetCandidate final : public ISingleAssetCandidate
		{
		public:
			explicit FEngineSingleAssetCandidate(DObject* InAsset)
				: AssetObject(InAsset), Package(InAsset ? InAsset->GetPackage() : nullptr) {}

			auto GetAsset() const -> DObject* override { return AssetObject; }
			auto GetPackage() const -> DPackage* override { return Package; }
			auto IsNewAsset() const -> bool override { return false; }
			auto GetAuthoredFingerprint() const -> std::string override
			{
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject))
					return Mesh->GetSourceImportData().SourceContentHash;
				if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					return Texture->GetDerivedDataKey();
				if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					return Cube->GetDerivedDataKey();
				if (const auto* Heightmap = Cast<DTerrainHeightmap>(AssetObject))
					return Heightmap->GetDerivedDataKey();
				return {};
			}
			auto Validate(std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				bool bValid = false;
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject)) bValid = Mesh->GetRenderData() != nullptr;
				else if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					bValid = Texture->GetPlatformData() != nullptr
						&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					bValid = Cube->GetPlatformData() != nullptr
						&& Cube->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Heightmap = Cast<DTerrainHeightmap>(AssetObject))
					bValid = Heightmap->GetPayload() != nullptr
						&& Heightmap->GetStatus() == ETerrainHeightmapStatus::Ready;
				if (!bValid)
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ValidationFailure,
						.Phase = "candidate-validation",
						.Message = "Engine asset candidate has no validated runtime data."});
				return bValid;
			}
			auto Abandon() noexcept -> void override
			{
				if (!Package) return;
				(void)Asset::DiscardUnpublishedPackage(Package);
				Package = nullptr;
				AssetObject = nullptr;
			}

		private:
			DObject* AssetObject = nullptr;
			DPackage* Package = nullptr;
		};

		template<typename T>
		class TNoFailExchange final : public IPreparedImportedStateExchange
		{
		public:
			TNoFailExchange(T& InTarget, T& InCandidate)
				: Target(&InTarget), Candidate(&InCandidate) {}
			auto Commit() noexcept -> void override
			{
				if (!bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = true; }
			}
			auto Reverse() noexcept -> void override
			{
				if (bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = false; }
			}
			auto Finalize() noexcept -> void override { Target = nullptr; Candidate = nullptr; }
		private:
			T* Target = nullptr;
			T* Candidate = nullptr;
			bool bCommitted = false;
		};

		class FNoopExchange final : public IPreparedImportedStateExchange
		{
		public:
			auto Commit() noexcept -> void override {}
			auto Reverse() noexcept -> void override {}
			auto Finalize() noexcept -> void override {}
		};

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

		class FIdentityProvider final : public IImportProvider
		{
		public:
			FIdentityProvider(std::string InId, uint32 InVersion, std::vector<std::string> InExtensions)
				: Id(std::move(InId)), Version(InVersion), Extensions(std::move(InExtensions)) {}
			auto GetProviderId() const -> std::string_view override { return Id; }
			auto GetContractVersion() const -> uint32 override { return Version; }
			auto CanImport(const FImportSourceRecognition& Source) const -> bool override
			{
				std::string Extension = Source.Extension;
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
					return static_cast<char>(std::tolower(Character));
				});
				return std::ranges::find(Extensions, Extension) != Extensions.end();
			}
			auto CaptureSettings(FImportPayload& OutSettings,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				OutSettings = MakePayload(std::format("Durin.{}.DefaultSettings", Id), 1, {});
				return true;
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>,
				FDependencyRequestSink&, std::vector<FImportDiagnostic>&) const -> bool override
			{
				return true;
			}
			auto Plan(const FSourceSnapshot&, const FImportPayload&, FImportPlanBuilder&,
				std::vector<FImportDiagnostic>&) const -> bool override { return true; }
		private:
			std::string Id;
			uint32 Version = 0;
			std::vector<std::string> Extensions;
		};

		auto MakeCapabilities(
			std::string ClassName,
			std::string ProviderId,
			bool bCanReimport,
			std::string ReplacementDescription) -> FSingleAssetCapabilitySet
		{
			FSingleAssetCapabilitySet Result{
				.AssetClassName = std::move(ClassName),
				.ProviderId = std::move(ProviderId)};
			Result.Capabilities = {
				{.Capability = ESingleAssetImportCapability::Import,
					.bAvailable = false, .Label = "Import as New Asset",
					.ReplacedStateDescription = "Creates a new authored asset."},
				{.Capability = ESingleAssetImportCapability::ReimportCurrentSource,
					.bAvailable = bCanReimport, .Label = "Reimport from Current Source",
					.ReplacedStateDescription = ReplacementDescription},
				{.Capability = ESingleAssetImportCapability::ReimportNewSource,
					.bAvailable = bCanReimport, .Label = "Reimport from Mounted Source",
					.ReplacedStateDescription = ReplacementDescription},
				{.Capability = ESingleAssetImportCapability::RepairSource,
					.bAvailable = true, .Label = "Repair Source Reference",
					.ReplacedStateDescription = "Replaces only persisted source identities, then rebuilds imported state."}};
			return Result;
		}

		class FStaticMeshHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override { return "Durin::DStaticMesh"; }
			auto GetProviderId() const -> std::string_view override { return "Assimp"; }
			auto InspectProvenance(const DObject& AssetObject, FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				const auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				if (!Mesh || !Mesh->GetSourceImportData().HasSource()) return false;
				const FStaticMeshSourceImportData& Source = Mesh->GetSourceImportData();
				Out = {
					.ProviderId = Source.ImporterId,
					.ProviderContractVersion = Source.ImporterVersion,
					.Settings = MakeStaticMeshSettings(Source.ImportSettings),
					.Sources = {{
						.StableIdentity = "geometry",
						.Role = "Geometry",
						.SourcePath = Source.SourcePath,
						.ContentHash = FXxHash128::FromString(Source.SourceContentHash)}},
					.AuthoredOutputFingerprint = Source.SourceContentHash};
				if (!Out.IsComplete() || Source.SourceContentHash.size() != 32)
				{
					Diagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidSource,
						.Phase = "provenance-inspection",
						.Message = "StaticMesh source provenance is incomplete."});
					return false;
				}
				return true;
			}
			auto QueryCapabilities(const DObject& AssetObject,
				const FSingleAssetProvenance&) const -> FSingleAssetCapabilitySet override
			{
				const auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				bool bGeometryOnly = Mesh != nullptr;
				if (bGeometryOnly && Mesh->GetPackage())
				{
					FAssetPath MeshPath;
					std::string IndexError;
					bGeometryOnly = FAssetPath::TryCreate(
						Mesh->GetPackage()->GetPackagePath(), MeshPath)
						&& GetImportRecordIndex().EnsureCurrent(IndexError)
						&& GetImportRecordIndex().FindManagers(MeshPath).empty();
				}
				auto Result = MakeCapabilities(std::string(GetAssetClassName()), "Assimp", bGeometryOnly,
					"Replaces StaticMesh geometry, material-slot import metadata, source provenance, DDC identity, and render resources; object identity and component overrides are retained.");
				if (!bGeometryOnly)
				{
					for (auto& Capability : Result.Capabilities)
						if (Capability.Capability == ESingleAssetImportCapability::ReimportCurrentSource
							|| Capability.Capability == ESingleAssetImportCapability::ReimportNewSource)
							Capability.Diagnostics.push_back({
								.Severity = EImportDiagnosticSeverity::Error,
								.Category = EImportDiagnosticCategory::CapabilityUnavailable,
								.Phase = "capability-query",
								.Message = "This StaticMesh is the root of a legacy multi-output import; use its Scene reimport action until record migration."});
				}
				return Result;
			}
			auto BuildCandidate(const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DStaticMesh>(Plan.GetAsset());
				const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
				FAssetPath CandidatePath;
				DStaticMesh* Candidate = nullptr;
				std::string Error;
				if (!Target || !Root || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				FImportedSceneData Scene;
				if (!ImportGeometryFromMemory(
					Root->GetBytes(), std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
					Scene, MakeMeshImportOptions(Target->GetImportSettings(), Root->SourcePath)))
				{
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build", .Message = "Captured geometry could not be decoded."});
					Result->Abandon();
					return nullptr;
				}
				Candidate->SeedMaterialReconciliationFrom(*Target);
				FStaticMeshSourceImportData Provenance{
					.SourcePath = Root->SourcePath,
					.SourceContentHash = Root->ContentHash.ToString(),
					.ImporterId = Plan.GetProvenance().ProviderId,
					.ImporterVersion = Plan.GetProvenance().ProviderContractVersion,
					.ImportSettings = Target->GetImportSettings()};
				Asset::Build::FStaticMeshBuildProduct Product;
				if (!Asset::Build::FStaticMeshBuildOperations::BuildImportedProduct(
						Asset::Build::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(*Candidate),
						MakeStaticMeshImportedData(Scene), Provenance,
						Root->SourcePath.Path, Product, Error)
					|| !Asset::Build::FStaticMeshBuildOperations::PublishImportedProduct(
						*Candidate, std::move(Product), Error))
				{
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build", .Message = Error});
					Result->Abandon();
					return nullptr;
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& TargetObject, ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DStaticMesh>(&TargetObject);
				auto* Candidate = Cast<DStaticMesh>(CandidateObject.GetAsset());
				std::string Error;
				auto Exchange = Target && Candidate
					? Target->PrepareImportedStateExchange(*Candidate, Error) : nullptr;
				if (!Exchange)
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "exchange-prepare", .Message = Error});
				return Exchange ? std::make_unique<FStaticMeshExchange>(std::move(Exchange)) : nullptr;
			}
			auto RepairSource(DObject& AssetObject, std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				std::string Error;
				const bool bResult = Mesh && Sources.size() == 1
					&& ChangeStaticMeshSourceReference(
						*Mesh, Sources[0].Path, Error);
				if (!bResult) Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair", .Message = Error});
				return bResult;
			}
		};

		class FTexture2DHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override { return "Durin::DTexture2D"; }
			auto GetProviderId() const -> std::string_view override { return "DurinImage"; }
			auto InspectProvenance(const DObject& AssetObject, FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				const auto* Texture = Cast<DTexture2D>(&AssetObject);
				if (!Texture || !Texture->GetSourceImportData().HasSource()) return false;
				const auto& Source = Texture->GetSourceImportData();
				Out = {.ProviderId = Source.DecoderId,
					.ProviderContractVersion = Source.DecoderVersion,
					.Settings = MakeTexture2DSettings(*Texture),
					.Sources = {{.StableIdentity = "image", .Role = "Image",
						.SourcePath = Source.Source.SourcePath,
						.ContentHash = MakeSourceHash(Source.Source)}},
					.AuthoredOutputFingerprint = Texture->GetDerivedDataKey()};
				return Out.IsComplete();
			}
			auto QueryCapabilities(const DObject&, const FSingleAssetProvenance&) const
				-> FSingleAssetCapabilitySet override
			{
				return MakeCapabilities(std::string(GetAssetClassName()), "DurinImage", true,
					"Replaces Texture2D decoded source, source provenance, import settings, mip/platform data, DDC identity, and render resource; object identity is retained.");
			}
			auto BuildCandidate(const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DTexture2D>(Plan.GetAsset());
				const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
				FAssetPath CandidatePath;
				DTexture2D* Candidate = nullptr;
				if (!Target || !Root || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				FTexture2DImportSettings Settings{
					.Usage = Target->GetUsage(),
					.CompressionQuality = Target->GetCompressionQuality(),
					.AlphaMipMode = Target->GetAlphaMipMode(),
					.AlphaCoverageThreshold = Target->GetAlphaCoverageThreshold(),
					.MaxResolution = Target->GetMaxResolution(),
					.bSRGB = Target->IsSRGB()};
				std::string Error;
				FEncodedSourceSnapshot Snapshot;
				UseCapturedSource(*Root, Snapshot);
				if (!BuildTexture2DCandidateFromSnapshot(
					*Candidate, Snapshot, Settings, Error))
				{
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build", .Message = Error});
					Result->Abandon();
					return nullptr;
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& TargetObject, ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DTexture2D>(&TargetObject);
				auto* Candidate = Cast<DTexture2D>(CandidateObject.GetAsset());
				return Target && Candidate ? std::make_unique<TNoFailExchange<DTexture2D>>(*Target, *Candidate) : nullptr;
			}
			auto RepairSource(DObject& AssetObject, std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Texture = Cast<DTexture2D>(&AssetObject);
				std::string Error;
				const bool bResult = Texture && Sources.size() == 1
					&& ChangeTexture2DSourceReference(
						*Texture, Sources[0].Path, Error);
				if (!bResult) Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair", .Message = Error});
				return bResult;
			}
		};

		class FTextureCubeHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override { return "Durin::DTextureCube"; }
			auto GetProviderId() const -> std::string_view override { return "DurinImage"; }
			auto InspectProvenance(const DObject& AssetObject, FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				const auto* Cube = Cast<DTextureCube>(&AssetObject);
				if (!Cube || !Cube->GetSourceImportData().HasSource()) return false;
				const auto& Source = Cube->GetSourceImportData();
				Out.ProviderId = Source.DecoderId;
				Out.ProviderContractVersion = Source.DecoderVersion;
				Out.Settings = MakeTextureCubeSettings(*Cube);
				Out.AuthoredOutputFingerprint = Cube->GetDerivedDataKey();
				if (Source.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
					Out.Sources.push_back({.StableIdentity = "panorama", .Role = "Panorama",
						.SourcePath = Source.Panorama.SourcePath,
						.ContentHash = MakeSourceHash(Source.Panorama)});
				else for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					const auto Face = static_cast<ETextureCubeFace>(Index);
					const FTextureSourceFile& FaceSource = Source.GetFace(Face);
					Out.Sources.push_back({.StableIdentity = std::format("face-{}", Index),
						.Role = "CubeFace", .SourcePath = FaceSource.SourcePath,
						.ContentHash = MakeSourceHash(FaceSource)});
				}
				return Out.IsComplete();
			}
			auto QueryCapabilities(const DObject&, const FSingleAssetProvenance&) const
				-> FSingleAssetCapabilitySet override
			{
				return MakeCapabilities(std::string(GetAssetClassName()), "DurinImage", true,
					"Replaces all TextureCube source provenance, panorama projection or six-face settings, mip/platform data, DDC identity, and render resource; object identity is retained.");
			}
			auto BuildCandidate(const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DTextureCube>(Plan.GetAsset());
				FAssetPath CandidatePath;
				DTextureCube* Candidate = nullptr;
				if (!Target || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				std::string Error;
				if (Target->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
					if (!Root)
					{
						Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::CandidateFailure,
							.Phase = "candidate-build", .Message = Error});
						Result->Abandon();
						return nullptr;
					}
					FEncodedSourceSnapshot Snapshot;
					UseCapturedSource(*Root, Snapshot);
					const FTextureCubePanoramaImportSettings Settings{
						Target->GetPanoramaFaceDimension(), Target->GetPanoramaExposureEV()};
					FTextureCubePanoramaSourceData Panorama;
					const bool bBuilt = TranslateTextureCubePanoramaSource(
						Snapshot.GetBytes(),
						std::filesystem::path(Snapshot.SourcePath.Path).extension().generic_string(),
						Panorama, Error)
						&& BuildAndPublishTextureCubePanorama(*Candidate, std::move(Panorama),
							Snapshot.ContentHash, Snapshot.SourcePath, Settings, Error);
					if (!bBuilt)
					{
						Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::CandidateFailure,
							.Phase = "candidate-build", .Message = Error});
						Result->Abandon();
						return nullptr;
					}
				}
				else
				{
					FTextureCubeSourceData SourceData;
					std::array<FXxHash128, TextureCubeFaceCount> Hashes;
					std::array<FSourcePath, TextureCubeFaceCount> Paths;
					std::array<FEncodedSourceSnapshot, TextureCubeFaceCount> Snapshots;
					std::array<std::span<const uint8>, TextureCubeFaceCount> EncodedFaces;
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
					{
						const FSourceSnapshotEntry* Entry = Plan.GetSnapshot().FindSource(
							Index == 0 ? "root" : std::format("face-{}", Index));
						if (!Entry) { Result->Abandon(); return nullptr; }
						UseCapturedSource(*Entry, Snapshots[Index]);
						EncodedFaces[Index] = Snapshots[Index].GetBytes();
						Hashes[Index] = Snapshots[Index].ContentHash;
						Paths[Index] = Entry->SourcePath;
					}
					if (!TranslateTextureCubeFaceSources(EncodedFaces, SourceData, Error)
						|| !BuildAndPublishTextureCubeFaces(
						*Candidate, std::move(SourceData), Hashes, Paths, {Target->IsSRGB()}, Error))
					{
						Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::CandidateFailure,
							.Phase = "candidate-build", .Message = Error});
						Result->Abandon();
						return nullptr;
					}
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& TargetObject, ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DTextureCube>(&TargetObject);
				auto* Candidate = Cast<DTextureCube>(CandidateObject.GetAsset());
				return Target && Candidate ? std::make_unique<TNoFailExchange<DTextureCube>>(*Target, *Candidate) : nullptr;
			}
			auto RepairSource(DObject& AssetObject, std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Cube = Cast<DTextureCube>(&AssetObject);
				std::string Error;
				bool bResult = false;
				if (Cube && Cube->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama
					&& Sources.size() == 1)
					bResult = ChangeTextureCubePanoramaSourceReference(
						*Cube, Sources[0].Path,
						{Cube->GetPanoramaFaceDimension(), Cube->GetPanoramaExposureEV()}, Error);
				else if (Cube && Sources.size() == TextureCubeFaceCount)
				{
					std::array<std::string, TextureCubeFaceCount> Paths;
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
						Paths[Index] = Sources[Index].Path;
					bResult = ChangeTextureCubeFaceSourceReferences(
						*Cube, Paths, {Cube->IsSRGB()}, Error);
				}
				if (!bResult) Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair", .Message = Error});
				return bResult;
			}
		};

		class FTerrainHeightmapHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override
			{
				return "Durin::DTerrainHeightmap";
			}
			auto GetProviderId() const -> std::string_view override { return "DurinImage"; }
			auto InspectProvenance(
				const DObject& AssetObject,
				FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				const auto* Heightmap = Cast<DTerrainHeightmap>(&AssetObject);
				if (!Heightmap || !Heightmap->GetSourceImportData().HasSource()) return false;
				const auto& Source = Heightmap->GetSourceImportData();
				Out = {
					.ProviderId = "DurinImage",
					.ProviderContractVersion = 1,
					.Settings = MakeTerrainHeightmapSettings(),
					.Sources = {{
						.StableIdentity = "heightmap",
						.Role = "Heightmap",
						.SourcePath = Source.SourcePath,
						.ContentHash = {
							.HashLow = Source.SourceContentHashLow,
							.HashHigh = Source.SourceContentHashHigh}}},
					.AuthoredOutputFingerprint = Heightmap->GetDerivedDataKey()};
				return Out.IsComplete();
			}
			auto QueryCapabilities(const DObject&, const FSingleAssetProvenance&) const
				-> FSingleAssetCapabilitySet override
			{
				return MakeCapabilities(std::string(GetAssetClassName()), "DurinImage", true,
					"Replaces exact height samples, hierarchy, source provenance, DDC identity, and revision while retaining object identity.");
			}
			auto BuildCandidate(
				const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DTerrainHeightmap>(Plan.GetAsset());
				const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
				FAssetPath CandidatePath;
				DTerrainHeightmap* Candidate = nullptr;
				if (!Target || !Root || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				std::string Error;
				FEncodedSourceSnapshot Snapshot;
				UseCapturedSource(*Root, Snapshot);
				FTerrainHeightmapSourceData SourceData;
				if (!TranslateTerrainHeightmapSource(
					std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
					Snapshot.GetBytes(), SourceData, Error))
				{
					Diagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build",
						.Message = Error});
					Result->Abandon();
					return nullptr;
				}
				if (!BuildTerrainHeightmapFromSource(
					*Candidate, std::move(SourceData), Snapshot, Error))
				{
					Diagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build",
						.Message = Error});
					Result->Abandon();
					return nullptr;
				}
				return Result;
			}
			auto PrepareImportedStateExchange(
				DObject& TargetObject,
				ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DTerrainHeightmap>(&TargetObject);
				auto* Candidate = Cast<DTerrainHeightmap>(CandidateObject.GetAsset());
				if (!Target || !Candidate) return nullptr;
				if (Target->IsSemanticImportNoOp(*Candidate))
					return std::make_unique<FNoopExchange>();
				Target->PrepareCandidateRevision(*Candidate);
				return std::make_unique<TNoFailExchange<DTerrainHeightmap>>(
					*Target, *Candidate);
			}
			auto RepairSource(
				DObject& AssetObject,
				std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Heightmap = Cast<DTerrainHeightmap>(&AssetObject);
				std::string Error;
				const bool bResult = Heightmap && Sources.size() == 1
					&& ChangeTerrainHeightmapSourceReference(
						*Heightmap, Sources[0].Path, Error);
				if (!bResult) Diagnostics.push_back({
					.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair",
					.Message = Error});
				return bResult;
			}
		};

		class FSceneRecordHandler final : public IImportRecordHandler
		{
		public:
			auto GetProviderId() const -> std::string_view override
			{
				return SceneImportProviderId;
			}

			auto QueryCapabilities(
				const DImportRecord&,
				const FImportRecordInspection& Inspection) const
				-> FImportRecordCapabilitySet override
			{
				const bool bCanPublish = !Inspection.bConflicted;
				auto Make = [&](EImportRecordAction Action, std::string Label,
					bool bAvailable) -> FImportRecordCapability
				{
					FImportRecordCapability Capability{
						.Action = Action,
						.bAvailable = bCanPublish && bAvailable,
						.Label = std::move(Label)};
					if (!Capability.bAvailable)
					{
						Capability.Diagnostics.push_back({
							.Severity = EImportDiagnosticSeverity::Error,
							.Category = Inspection.bConflicted
								? EImportDiagnosticCategory::Collision
								: EImportDiagnosticCategory::CapabilityUnavailable,
							.Phase = "capability-query",
							.SourceIdentity = "root",
							.OutputIdentity = "record",
							.Message = Inspection.bConflicted
								? "Repair the duplicated record identity or manager conflict first."
								: "The selected record does not require this action."});
						FinalizeImportDiagnostics(Capability.Diagnostics, "capability-query");
					}
					return Capability;
				};
				return {
					.ProviderId = std::string(SceneImportProviderId),
					.Capabilities = {
						Make(EImportRecordAction::Reimport, "Reimport Managed Outputs", true),
						Make(EImportRecordAction::RecreateMissingOutputs,
							"Recreate Missing Outputs", Inspection.bHasMissingManagedOutput),
						Make(EImportRecordAction::RepairManagedOutputs,
							"Repair Changed Outputs", Inspection.bHasFingerprintMismatch)}};
			}

			auto Execute(
				DImportRecord& Record,
				EImportRecordAction Action,
				const FMultiOutputExecutionOptions& Options) const
				-> FImportRecordActionResult override
			{
				FImportRecordActionResult Result;
				const bool bRecreate = Action != EImportRecordAction::Reimport;
				const FSceneImportPlanResult Planned =
					PlanSceneReimport(Record, bRecreate, Options.Progress);
				if (!Planned)
				{
					Result.Message = Planned.Message;
					Result.Diagnostics = Planned.Diagnostics;
					return Result;
				}
				FSceneImportExecutionResult Executed = ExecuteSceneImport(Planned.Plan, Options);
				Result.bSucceeded = Executed.bSucceeded;
				Result.Message = std::move(Executed.Message);
				Result.Record = Executed.Record;
				Result.Orphans = std::move(Executed.OrphanedAssets);
				Result.Diagnostics = std::move(Executed.Diagnostics);
				Result.Provider = std::move(Executed.Provider);
				std::unordered_map<std::string, DObject*> OutputByPath;
				auto IndexOutput = [&](DObject* Output) {
					if (Output && Output->GetPackage())
						OutputByPath.emplace(Output->GetPackage()->GetPackagePath(), Output);
				};
				for (DStaticMesh* Mesh : Executed.Meshes) IndexOutput(Mesh);
				for (DSkeleton* Skeleton : Executed.Skeletons) IndexOutput(Skeleton);
				for (DSkeletalMesh* Mesh : Executed.SkeletalMeshes) IndexOutput(Mesh);
				for (DAnimationClip* Clip : Executed.AnimationClips) IndexOutput(Clip);
				for (DMaterialInstance* Material : Executed.Materials) IndexOutput(Material);
				for (DTexture2D* Texture : Executed.Textures) IndexOutput(Texture);
				if (Result.Record)
					for (const FImportRecordOutput& Output : Result.Record->GetOutputs())
					{
						const auto Found = OutputByPath.find(Output.AssetPath.ToString());
						if (Found != OutputByPath.end()) Result.Outputs.push_back(Found->second);
					}
				return Result;
			}
		};

		std::mutex GRegistrationMutex;
		bool GRegistered = false;
	}

		auto InspectStaticMeshSource(
			const DStaticMesh& Mesh) -> FStaticMeshSourceDiagnostic
		{
			const FStaticMeshSourceImportData& Source = Mesh.GetSourceImportData();
			if (!Source.HasSource()) return {};
			std::filesystem::path PhysicalPath;
			std::string Error;
			if (!ResolvePortableStaticMeshSource(Mesh, PhysicalPath, Error))
				return {EStaticMeshSourceStatus::Invalid, {}, std::move(Error)};
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				return {
					EStaticMeshSourceStatus::Missing,
					PhysicalPath.generic_string(),
					std::format(
						"Static mesh source is missing: {}. Use source-path repair to select its replacement.",
						Source.SourcePath.Path)};
			}
			std::string CurrentHash;
			if (!HashStaticMeshSource(PhysicalPath, CurrentHash, Error))
				return {
					EStaticMeshSourceStatus::Invalid,
					PhysicalPath.generic_string(),
					std::move(Error)};
			if (!Source.SourceContentHash.empty()
				&& CurrentHash != Source.SourceContentHash)
			{
				return {
					EStaticMeshSourceStatus::Changed,
					PhysicalPath.generic_string(),
					"The mounted static-mesh source bytes changed since this asset was last imported."};
			}
			return {EStaticMeshSourceStatus::Available, PhysicalPath.generic_string(), {}};
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
			FMountedSourceFile Source;
			if (!ResolveMountedSourceReference(
				Mesh.GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError))
				return false;
			std::string SourceHash;
			if (!HashStaticMeshSource(Source.PhysicalPath, SourceHash, OutError)) return false;

			FStaticMeshSourceImportData NewSource = {
				.SourcePath = std::move(Source.SourcePath),
				.SourceContentHash = std::move(SourceHash),
				.ImporterId = std::string(StaticMeshImporterId),
				.ImporterVersion = StaticMeshAssimpImporterVersion,
				.ImportSettings = Mesh.GetImportSettings()};
			FStaticMeshAuthoringProduct Product;
			return BuildStaticMeshFileProduct(
					Mesh, Source.PhysicalPath.generic_string(), std::move(NewSource),
					Source.PhysicalPath.generic_string(), Product, OutError)
				&& Mesh.PublishImportedProduct(std::move(Product), OutError);
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
			FMountedSourceFile Source;
			if (!PrepareMountedSourceFile(
				FilePath, Mesh.GetPackage()->GetPackagePath(),
				TargetSourceVirtualPath, Source, OutError)) return false;
			const bool bChanged = ChangeStaticMeshSourceReference(
				Mesh, Source.SourcePath.Path, OutError);
			if (bChanged)
				CommitMountedSourceFile(Source);
			else
				RollbackMountedSourceFile(Source);
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
				|| Asset::FindLoadedPackage(ParsedAssetPath)
				|| Asset::FindDraftPackage(ParsedAssetPath))
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
			FMountedSourceFile MountedSource;
			if (!PrepareMountedSourceFile(
				Input, ParsedAssetPath.ToString(), StoredSourcePath, MountedSource, Error,
				bEngineAuthoringContext
					? EMountedSourceMutationContext::EngineAuthoring
					: EMountedSourceMutationContext::DependencySafe))
				return {false, std::move(Error), nullptr};
			Destination = MountedSource.PhysicalPath;
			StoredSourcePath = MountedSource.SourcePath.Path;
			std::string SourceHash;
			if (!HashStaticMeshSource(Destination, SourceHash, Error))
			{
				RollbackMountedSourceFile(MountedSource);
				return {false, std::move(Error), nullptr};
			}

			DStaticMesh* Mesh = nullptr;
			const Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Mesh);
			if (!CreateResult)
			{
				RollbackMountedSourceFile(MountedSource);
				return {false, CreateResult.Message, nullptr};
			}
			FStaticMeshAuthoringProduct Product;
			if (!BuildStaticMeshFileProduct(
					*Mesh, Destination.generic_string(),
					{
						.SourcePath = {.Path = StoredSourcePath},
						.SourceContentHash = std::move(SourceHash),
						.ImporterId = std::string(StaticMeshImporterId),
						.ImporterVersion = StaticMeshAssimpImporterVersion,
						.ImportSettings = ImportSettings},
					Destination.generic_string(), Product, Error)
				|| !Mesh->PublishImportedProduct(std::move(Product), Error))
			{
				RollbackMountedSourceFile(MountedSource);
				Asset::DiscardUnpublishedPackage(Mesh->GetPackage());
				return {false, std::move(Error), nullptr};
			}

			const Asset::FAssetResult SaveResult = Asset::SavePackage(Mesh->GetPackage());
			if (!SaveResult)
			{
				RollbackMountedSourceFile(MountedSource);
				Asset::DiscardUnpublishedPackage(Mesh->GetPackage());
				return {false, SaveResult.Message, nullptr};
			}
			CommitMountedSourceFile(MountedSource);
			return {true, {}, Mesh};
		}
	auto FStandardAssetAuthoringFeatures::BuildFileProduct(
		DStaticMesh& Mesh,
		std::string_view SourcePath,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceContentHash,
		FStaticMeshAuthoringProduct& OutProduct,
		std::string& OutError) -> bool
	{
		return BuildStaticMeshFileProduct(
			Mesh, SourcePath, std::move(SourceImportData), SourceContentHash, OutProduct, OutError);
	}

	auto FStandardAssetAuthoringFeatures::PostLoadUncooked(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		return PostLoadStaticMesh(Mesh, OutDiagnostic, OutError);
	}

	auto FStandardAssetAuthoringFeatures::ChangeSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return ChangeStaticMeshSourceReference(Mesh, SourceVirtualPath, OutError);
	}

	auto FStandardAssetAuthoringFeatures::PostLoadUncooked(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return PostLoadTexture2DFeature(Texture, OutError);
	}

	auto FStandardAssetAuthoringFeatures::PostLoadUncooked(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeFeature(Texture, OutError);
	}

	auto RegisterStandardAssetImportProviders(
		std::string& OutError, FModuleOwnedCallbackGate OwnerGate) -> bool
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (GRegistered) { OutError.clear(); return true; }
		OpenAsyncImportProviderAdmission(SceneImportProviderId);
		OpenAsyncImportProviderAdmission("Assimp");
		OpenAsyncImportProviderAdmission("DurinImage");
		auto& Providers = GetProviderRegistry();
		auto& Handlers = GetSingleAssetHandlerRegistry();
		auto& RecordHandlers = GetImportRecordHandlerRegistry();
		auto RollbackFrameworkRegistrations = [&] {
			Handlers.Unregister("Durin::DStaticMesh");
			Handlers.Unregister("Durin::DTexture2D");
			Handlers.Unregister("Durin::DTextureCube");
			Handlers.Unregister("Durin::DTerrainHeightmap");
			RecordHandlers.Unregister(SceneImportProviderId);
			Providers.Unregister("DurinImage");
			Providers.Unregister("Assimp");
			Providers.Unregister(SceneImportProviderId);
		};
		if (!Providers.Register(CreateSceneImportProvider(), OwnerGate, OutError))
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		if (!RecordHandlers.Register(
			std::make_shared<FSceneRecordHandler>(), OwnerGate, OutError))
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		if (!Providers.Register(std::make_shared<FIdentityProvider>("Assimp", 3,
			std::vector<std::string>{
				".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"}),
			OwnerGate, OutError))
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		if (!Providers.Register(std::make_shared<FIdentityProvider>("DurinImage", 1,
			std::vector<std::string>{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"}),
			OwnerGate, OutError))
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		std::vector<std::shared_ptr<ISingleAssetImportHandler>> Builtins = {
			std::make_shared<FStaticMeshHandler>(),
			std::make_shared<FTexture2DHandler>(),
			std::make_shared<FTextureCubeHandler>(),
			std::make_shared<FTerrainHeightmapHandler>()};
		for (const auto& Handler : Builtins)
		{
			if (Handlers.Register(Handler, OwnerGate, OutError)) continue;
			RollbackFrameworkRegistrations();
			return false;
		}
		if (!RegisterTexture2DPropertyEditing())
		{
			RollbackFrameworkRegistrations();
			OutError = "Failed to register Texture2D property authoring policy.";
			return false;
		}
		if (!RegisterTexture2DSourceRelocation())
		{
			UnregisterTexture2DPropertyEditing();
			RollbackFrameworkRegistrations();
			OutError = "Failed to register Texture2D source relocation policy.";
			return false;
		}
		GRegistered = true;
		OutError.clear();
		return true;
	}

	auto UnregisterStandardAssetImportProviders() -> void
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (!GRegistered) return;
		UnregisterTexture2DSourceRelocation();
		UnregisterTexture2DPropertyEditing();
		CancelAndDrainAsyncImportsForProvider(SceneImportProviderId);
		CancelAndDrainAsyncImportsForProvider("Assimp");
		CancelAndDrainAsyncImportsForProvider("DurinImage");
		auto& Handlers = GetSingleAssetHandlerRegistry();
		Handlers.Unregister("Durin::DStaticMesh");
		Handlers.Unregister("Durin::DTexture2D");
		Handlers.Unregister("Durin::DTextureCube");
		Handlers.Unregister("Durin::DTerrainHeightmap");
		GetImportRecordHandlerRegistry().Unregister(SceneImportProviderId);
		auto& Providers = GetProviderRegistry();
		Providers.Unregister("DurinImage");
		Providers.Unregister("Assimp");
		Providers.Unregister(SceneImportProviderId);
		GRegistered = false;
	}
}
