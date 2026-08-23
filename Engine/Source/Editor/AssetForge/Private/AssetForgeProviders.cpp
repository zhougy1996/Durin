#include "AssetForgeProviders.h"
#include "ImportService.h"
#include "AssetForgeAuthoringFeatures.h"
#include "StaticMeshSourceTranslation.h"
#include "Texture2DSourceTranslation.h"
#include "Texture2DBuildAdapter.h"
#include "TextureCubeSourceTranslation.h"
#include "VolumeTextureSourceTranslation.h"
#include "Texture2DPostLoad.h"
#include "TextureCubePostLoadPolicy.h"
#include "TerrainHeightmapSourceTranslation.h"

#include "Animation/AnimationClip.h"
#include "ImportedScene.h"
#include "Asset/MountedSource.h"
#include "AssetImportCore.h"
#include "AssetAuthoring.h"
#include "DObject/ObjectLifecycle.h"
#include "EncodedSourceSnapshot.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SceneImport.h"
#include "SceneImportInternal.h"
#include "AsyncImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/VolumeTexture.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "TextureCubeBuildAdapter.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "TerrainHeightmapBuildAdapter.h"
#include "ImageFamilyInterchange.h"

namespace Durin::Asset::Forge
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

		auto HashStaticMeshSource(
			const std::filesystem::path& Path,
			std::string& OutHash,
			std::string& OutError) -> bool
		{
			std::vector<std::byte> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path))
			{
				OutError = std::format(
					"Failed to read static mesh source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes).ToString();
			return true;
		}

		template<typename T>
		auto AppendValue(std::vector<std::byte>& Bytes, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const std::span<const std::byte> ValueBytes = std::as_bytes(std::span{&Value, 1});
			Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
		}

		auto AppendString(std::vector<std::byte>& Bytes, std::string_view Value) -> void
		{
			const uint64 Size = Value.size();
			AppendValue(Bytes, Size);
			const auto ValueBytes = std::as_bytes(std::span(Value));
			Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
		}

		template<typename T>
		auto ReadValue(std::span<const std::byte>& Bytes, T& OutValue) -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Bytes.size() < sizeof(T)) return false;
			std::memcpy(&OutValue, Bytes.data(), sizeof(T));
			Bytes = Bytes.subspan(sizeof(T));
			return true;
		}

		auto ReadString(std::span<const std::byte>& Bytes, std::string& OutValue) -> bool
		{
			uint64 Size = 0;
			if (!ReadValue(Bytes, Size) || Size > Bytes.size() || Size > 1'024 * 1'024)
				return false;
			OutValue.assign(reinterpret_cast<const char*>(Bytes.data()),
				static_cast<size_t>(Size));
			Bytes = Bytes.subspan(static_cast<size_t>(Size));
			return true;
		}

		template<typename T>
		auto AppendTrivialVector(std::vector<std::byte>& Bytes,
			std::span<const T> Values) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const uint64 Count = Values.size();
			AppendValue(Bytes, Count);
			const auto ValueBytes = std::as_bytes(Values);
			Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
		}

		template<typename T>
		auto ReadTrivialVector(std::span<const std::byte>& Bytes,
			std::vector<T>& OutValues, uint64 MaximumCount) -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			uint64 Count = 0;
			if (!ReadValue(Bytes, Count) || Count > MaximumCount
				|| Count > Bytes.size() / sizeof(T)) return false;
			OutValues.resize(static_cast<size_t>(Count));
			const size_t ByteCount = static_cast<size_t>(Count) * sizeof(T);
			std::memcpy(OutValues.data(), Bytes.data(), ByteCount);
			Bytes = Bytes.subspan(ByteCount);
			return true;
		}

		auto MakePayload(std::string SchemaId, uint32 Version, std::vector<std::byte> Bytes)
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

		auto MakeInterchangePayload(
			std::string SchemaId, uint32 Version, std::vector<std::byte> Bytes)
			-> FInterchangePayload
		{
			FInterchangePayload Payload{
				.SchemaId = std::move(SchemaId),
				.SchemaVersion = Version,
				.Bytes = std::move(Bytes)};
			std::string Error;
			requiref(Payload.Finalize(Error), "{}", Error);
			return Payload;
		}

		auto MakeStaticMeshSettings(const FStaticMeshImportSettings& Settings) -> FImportPayload
		{
			std::vector<std::byte> Bytes;
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
				std::vector<std::byte> Bytes;
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

			FAssetPath Destination;
			if (!Mesh.GetPackage()
				|| !FAssetPath::TryCreate(Mesh.GetPackage()->GetPackagePath(), Destination, &OutError))
				return false;
			FInterchangeProvenance Existing;
			std::optional<FInterchangeProvenance> Provenance;
			if (InspectStaticMeshInterchangeProvenance(Mesh, Existing, OutError))
				Provenance = std::move(Existing);
			else OutError.clear();
			FInterchangeImportRequest Request;
			if (!MakeStaticMeshInterchangeRequest(Source.SourcePath, Destination,
				Source.ImportSettings, EInterchangeImportMode::Recover,
				{.OwnerId = std::format("StaticMesh.Recovery:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Provenance), Request, OutError)) return false;
			Request.Lifetime = EImportOperationLifetime::SessionCritical;
			const FInterchangeImportHandle Handle = GetImportService().SubmitInterchangeImport(
				std::move(Request), std::format("Recover StaticMesh {}", Destination.GetAssetName()));
			if (!Handle)
			{
				OutError = "StaticMesh Interchange recovery could not be submitted.";
				return false;
			}
			OutDiagnostic = {
				.Status = EStaticMeshDerivedDataStatus::Missing,
				.Message = "Scheduled SessionCritical StaticMesh Interchange recovery.",
				.bSourceImporterInvoked = true};
			OutError.clear();
			return true;
		}


		auto MakeTexture2DSettings(const DTexture2D& Texture) -> FImportPayload
		{
			std::vector<std::byte> Bytes;
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
			std::vector<std::byte> Bytes;
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
				if (!Asset::FindResidentPackage(OutPath)
					&& !Asset::FindAssetExact(OutPath)) return true;
			}
			return false;
		}

		class FEngineSingleAssetCandidate final : public ISingleAssetCandidate
		{
		public:
			explicit FEngineSingleAssetCandidate(DObject* InAsset, bool bInNewAsset = false)
				: AssetObject(InAsset), Package(InAsset ? InAsset->GetPackage() : nullptr),
				bNewAsset(bInNewAsset) {}

			auto GetAsset() const -> DObject* override { return AssetObject; }
			auto GetPackage() const -> DPackage* override { return Package; }
			auto IsNewAsset() const -> bool override { return bNewAsset; }
			auto GetAuthoredFingerprint() const -> std::string override
			{
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject))
					return Mesh->GetSourceImportData().SourceContentHash;
				if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					return Texture->GetDerivedDataKey();
				if (const auto* Volume = Cast<DVolumeTexture>(AssetObject))
					return Volume->GetDerivedDataKey();
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
				else if (const auto* Volume = Cast<DVolumeTexture>(AssetObject))
					bValid = Volume->GetPlatformData() != nullptr
						&& Volume->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					bValid = Cube->GetPlatformData() != nullptr
						&& Cube->GetBuildStatus() == ETextureBuildStatus::Ready;
					else if (const auto* Heightmap = Cast<DTerrainHeightmap>(AssetObject))
						bValid = Heightmap->GetPayload() != nullptr
							&& Heightmap->GetStatus() == ETerrainHeightmapStatus::Ready;
					else if (const auto* Skeleton = Cast<DSkeleton>(AssetObject))
					{
						std::string Error;
						bValid = Skeleton->Validate(Error);
					}
					else if (const auto* Mesh = Cast<DSkeletalMesh>(AssetObject))
					{
						std::string Error;
						bValid = Mesh->Validate(Error);
					}
					else if (const auto* Clip = Cast<DAnimationClip>(AssetObject))
					{
						std::string Error;
						bValid = Clip->Validate(Error);
					}
					else if (const auto* Record = Cast<Asset::DImportRecord>(AssetObject))
					{
						std::string Error;
						bValid = Record->Validate(Error);
					}
					else if (Cast<DMaterialInstance>(AssetObject)) bValid = true;
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
				if (DPackage* Detached = DetachPackageForAbandon())
					(void)Asset::UnloadPackage(Detached,
						Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			}
			auto DetachPackageForAbandon() noexcept -> DPackage* override
			{
				DPackage* Detached = Package;
				Package = nullptr;
				AssetObject = nullptr;
				return Detached;
			}

		private:
			DObject* AssetObject = nullptr;
			DPackage* Package = nullptr;
			bool bNewAsset = false;
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

		inline constexpr std::string_view ImageTranslatorId = "Durin.Image";
		inline constexpr std::string_view Texture2DPipelineId = "Durin.Texture2D.Default";
		inline constexpr std::string_view Texture2DFactoryId = "Durin.Texture2D.Factory";
		inline constexpr std::string_view ImageNodeSchema = "Durin.Image.RGBA8";
		inline constexpr std::string_view Texture2DPlanSchema = "Durin.Texture2D.Plan";
		inline constexpr std::string_view EmptyTranslatorSettingsSchema =
			"Durin.Image.TranslatorSettings";
		inline constexpr std::string_view GeometryTranslatorId = "Durin.Geometry";
		inline constexpr std::string_view StaticMeshPipelineId = "Durin.StaticMesh.Default";
		inline constexpr std::string_view StaticMeshFactoryId = "Durin.StaticMesh.Factory";
		inline constexpr std::string_view StaticMeshNodeSchema =
			"Durin.Geometry.StaticMesh";
		inline constexpr std::string_view StaticMeshPlanSchema = "Durin.StaticMesh.Plan";
		inline constexpr std::string_view GeometryTranslatorSettingsSchema =
			"Durin.Geometry.TranslatorSettings";
		inline constexpr std::string_view SceneTranslatorId = "Durin.SceneGraph";
		inline constexpr std::string_view ScenePipelineId = "Durin.Scene.Default";
		inline constexpr std::string_view ScenePlanSchema = "Durin.Scene.Plan";
		inline constexpr std::string_view SceneNodeSchema = "Durin.Scene.Node";
		inline constexpr std::array<std::string_view, 7> SceneFactoryIds{
			"Durin.Scene.StaticMesh.Factory", "Durin.Scene.Material.Factory",
			"Durin.Scene.Skeleton.Factory", "Durin.Scene.SkeletalMesh.Factory",
			"Durin.Scene.AnimationClip.Factory", "Durin.Scene.Texture2D.Factory",
			"Durin.Scene.ImportRecord.Factory"};

		struct FSceneInterchangePlan
		{
			FAssetPath DestinationDirectory;
			FStaticMeshImportSettings MeshSettings;
			EImportOutputPolicy DefaultPolicy = EImportOutputPolicy::Create;
			std::vector<FInterchangeOutputMapping> ExistingMappings;
		};

		struct FSceneInterchangeCachedPlan
		{
			std::shared_ptr<const FSceneProviderPlanData> Data;
			std::shared_ptr<const FSourceSnapshot> Snapshot;
			std::vector<FImportOutputPreview> Outputs;
		};

		std::mutex GSceneInterchangeCacheMutex;
		std::unordered_map<std::string, std::shared_ptr<const FSceneInterchangeCachedPlan>>
			GSceneInterchangeCache;
		std::unordered_map<std::string, std::vector<FImportOutputPreview>>
			GSceneInterchangeOutputCache;

		auto MakeSceneOutputCacheKey(
			std::string_view SourceKey, const FAssetPath& Destination) -> std::string
		{
			return std::format("{}|{}", SourceKey, Destination.ToString());
		}

		auto EncodeSceneInterchangePlan(const FSceneInterchangePlan& Plan)
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.DestinationDirectory.ToString());
			AppendValue(Bytes, Plan.DefaultPolicy);
			AppendValue(Bytes, Plan.MeshSettings.ForwardAxis);
			AppendValue(Bytes, Plan.MeshSettings.RightAxis);
			AppendValue(Bytes, Plan.MeshSettings.UpAxis);
			AppendValue(Bytes, static_cast<uint64>(Plan.ExistingMappings.size()));
			for (const FInterchangeOutputMapping& Mapping : Plan.ExistingMappings)
			{
				AppendString(Bytes, Mapping.TranslatedNodeIdentity);
				AppendString(Bytes, Mapping.OutputIdentity);
				AppendString(Bytes, Mapping.AssetPath.ToString());
			}
			return MakeInterchangePayload(std::string(ScenePlanSchema), 1, std::move(Bytes));
		}

		auto EncodeSceneAuthoredSettings(const FSceneInterchangePlan& Plan)
			-> FInterchangePayload
		{
			return EncodeSceneInterchangePlan({
				.DestinationDirectory = Plan.DestinationDirectory,
				.MeshSettings = Plan.MeshSettings,
				.DefaultPolicy = EImportOutputPolicy::Create});
		}

		auto DecodeSceneInterchangePlan(
			const FInterchangePayload& Payload,
			FSceneInterchangePlan& OutPlan,
			std::string& OutError) -> bool
		{
			std::string Destination;
			std::span<const std::byte> Bytes(Payload.Bytes);
			if (Payload.SchemaId != ScenePlanSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes)
				|| !ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, OutPlan.DefaultPolicy)
				|| !ReadValue(Bytes, OutPlan.MeshSettings.ForwardAxis)
				|| !ReadValue(Bytes, OutPlan.MeshSettings.RightAxis)
				|| !ReadValue(Bytes, OutPlan.MeshSettings.UpAxis)
				|| !FAssetPath::TryCreate(Destination, OutPlan.DestinationDirectory, &OutError)
				|| !OutPlan.MeshSettings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "Scene Interchange plan payload is malformed.";
				return false;
			}
			uint64 MappingCount = 0;
			if (!ReadValue(Bytes, MappingCount) || MappingCount > MaximumImportRecordOutputs)
			{
				OutError = "Scene Interchange output mapping count is invalid.";
				return false;
			}
			OutPlan.ExistingMappings.clear();
			OutPlan.ExistingMappings.reserve(static_cast<size_t>(MappingCount));
			for (uint64 Index = 0; Index < MappingCount; ++Index)
			{
				std::string TranslatedIdentity;
				std::string OutputIdentity;
				std::string AssetPath;
				FAssetPath ParsedPath;
				if (!ReadString(Bytes, TranslatedIdentity) || !ReadString(Bytes, OutputIdentity)
					|| !ReadString(Bytes, AssetPath)
					|| !FAssetPath::TryCreate(AssetPath, ParsedPath, &OutError)) return false;
				OutPlan.ExistingMappings.push_back({.TranslatedNodeIdentity = std::move(TranslatedIdentity),
					.OutputIdentity = std::move(OutputIdentity), .AssetPath = std::move(ParsedPath)});
			}
			if (!Bytes.empty()) { OutError = "Scene Interchange plan has trailing bytes."; return false; }
			OutError.clear();
			return true;
		}

		auto EncodeSceneNodeReference(std::string_view CacheKey, uint32 OutputIndex)
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, CacheKey);
			AppendValue(Bytes, OutputIndex);
			return MakeInterchangePayload(std::string(SceneNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeSceneNodeReference(
			const FInterchangePayload& Payload,
			std::shared_ptr<const FSceneInterchangeCachedPlan>& OutPlan,
			const FSceneOutputData*& OutOutput,
			std::string& OutError) -> bool
		{
			std::span<const std::byte> Bytes(Payload.Bytes);
			std::string Key;
			uint32 Index = 0;
			if (Payload.SchemaId != SceneNodeSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes)
				|| !ReadString(Bytes, Key) || !ReadValue(Bytes, Index) || !Bytes.empty())
			{
				OutError = "Scene source-node reference is malformed.";
				return false;
			}
			{
				std::lock_guard Lock(GSceneInterchangeCacheMutex);
				const auto It = GSceneInterchangeCache.find(Key);
				if (It != GSceneInterchangeCache.end()) OutPlan = It->second;
			}
			if (!OutPlan || !OutPlan->Data || Index >= OutPlan->Data->Outputs.size())
			{
				OutError = "Scene source-node immutable value is no longer available.";
				return false;
			}
			OutOutput = &OutPlan->Data->Outputs[Index];
			OutError.clear();
			return true;
		}

		auto SceneNodeKind(ESceneOutputKind Kind) -> std::string_view
		{
			switch (Kind)
			{
			case ESceneOutputKind::StaticMesh: return "Durin.Scene.StaticMesh";
			case ESceneOutputKind::MaterialInstance: return "Durin.Scene.Material";
			case ESceneOutputKind::Skeleton: return "Durin.Scene.Skeleton";
			case ESceneOutputKind::SkeletalMesh: return "Durin.Scene.SkeletalMesh";
			case ESceneOutputKind::AnimationClip: return "Durin.Scene.AnimationClip";
			case ESceneOutputKind::Texture2D: return "Durin.Scene.Texture2D";
			case ESceneOutputKind::ImportRecord: return "Durin.Scene.ImportRecord";
			}
			return {};
		}

		auto SceneFactoryIndex(ESceneOutputKind Kind) -> size_t
		{
			switch (Kind)
			{
			case ESceneOutputKind::StaticMesh: return 0;
			case ESceneOutputKind::MaterialInstance: return 1;
			case ESceneOutputKind::Skeleton: return 2;
			case ESceneOutputKind::SkeletalMesh: return 3;
			case ESceneOutputKind::AnimationClip: return 4;
			case ESceneOutputKind::Texture2D: return 5;
			case ESceneOutputKind::ImportRecord: return 6;
			}
			return 0;
		}

		auto SceneOutputClassName(ESceneOutputKind Kind) -> std::string_view
		{
			switch (Kind)
			{
			case ESceneOutputKind::StaticMesh: return "Durin::DStaticMesh";
			case ESceneOutputKind::MaterialInstance: return "Durin::DMaterialInstance";
			case ESceneOutputKind::Skeleton: return "Durin::DSkeleton";
			case ESceneOutputKind::SkeletalMesh: return "Durin::DSkeletalMesh";
			case ESceneOutputKind::AnimationClip: return "Durin::DAnimationClip";
			case ESceneOutputKind::Texture2D: return "Durin::DTexture2D";
			case ESceneOutputKind::ImportRecord: return "Durin::Asset::DImportRecord";
			}
			return {};
		}

		struct FDecodedImageInterchangeValue
		{
			FTextureSourceData SourceData;
			FSourcePath SourcePath;
			FXxHash128 SourceHash{};
			uint64 SourceFileSize = 0;
			int64 SourceLastWriteTime = 0;
		};

		struct FTexture2DInterchangePlan
		{
			FAssetPath Destination;
			FTexture2DImportSettings Settings;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		};

		struct FDecodedStaticMeshInterchangeValue
		{
			Asset::Build::FStaticMeshImportedData ImportedData;
			FSourcePath SourcePath;
			FXxHash128 SourceHash{};
		};

		struct FStaticMeshInterchangePlan
		{
			FAssetPath Destination;
			FStaticMeshImportSettings Settings;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		};

		auto EncodeGeometryTranslatorSettings(const FStaticMeshImportSettings& Settings)
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Settings.ForwardAxis);
			AppendValue(Bytes, Settings.RightAxis);
			AppendValue(Bytes, Settings.UpAxis);
			return MakeInterchangePayload(
				std::string(GeometryTranslatorSettingsSchema), 1, std::move(Bytes));
		}

		auto DecodeGeometryTranslatorSettings(
			const FInterchangePayload& Payload,
			FStaticMeshImportSettings& OutSettings,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != GeometryTranslatorSettingsSchema
				|| Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "Geometry translator settings schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			if (!ReadValue(Bytes, OutSettings.ForwardAxis)
				|| !ReadValue(Bytes, OutSettings.RightAxis)
				|| !ReadValue(Bytes, OutSettings.UpAxis)
				|| !Bytes.empty() || !OutSettings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "Geometry translator settings are malformed.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeStaticMeshInterchangeValue(
			const FDecodedStaticMeshInterchangeValue& Value) -> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Value.SourcePath.Path);
			AppendValue(Bytes, Value.SourceHash.HashLow);
			AppendValue(Bytes, Value.SourceHash.HashHigh);
			AppendValue(Bytes, static_cast<uint64>(Value.ImportedData.MaterialSlots.size()));
			for (const Asset::Build::FStaticMeshImportedMaterialSlot& Slot
				: Value.ImportedData.MaterialSlots)
			{
				AppendString(Bytes, Slot.Name);
				AppendValue(Bytes, Slot.SourceMaterialIndex);
				AppendString(Bytes, Slot.SourceName);
			}
			AppendValue(Bytes, static_cast<uint64>(Value.ImportedData.Meshes.size()));
			for (const Asset::Build::FStaticMeshImportedMesh& Mesh
				: Value.ImportedData.Meshes)
			{
				AppendString(Bytes, Mesh.Name);
				AppendValue(Bytes, Mesh.SourceMaterialIndex);
				AppendTrivialVector(Bytes, std::span(Mesh.Positions));
				AppendTrivialVector(Bytes, std::span(Mesh.Normals));
				AppendTrivialVector(Bytes, std::span(Mesh.Tangents));
				for (const auto& UVs : Mesh.UVChannels)
					AppendTrivialVector(Bytes, std::span(UVs));
				AppendTrivialVector(Bytes, std::span(Mesh.Colors));
				AppendTrivialVector(Bytes, std::span(Mesh.Indices));
			}
			return MakeInterchangePayload(
				std::string(StaticMeshNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeStaticMeshInterchangeValue(
			const FInterchangePayload& Payload,
			FDecodedStaticMeshInterchangeValue& OutValue,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != StaticMeshNodeSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "StaticMesh source-node payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			uint64 SlotCount = 0;
			uint64 MeshCount = 0;
			if (!ReadString(Bytes, OutValue.SourcePath.Path)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashLow)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashHigh)
				|| !ReadValue(Bytes, SlotCount) || SlotCount > MaxImportedSourceMaterials)
			{
				OutError = "StaticMesh source-node payload header is malformed.";
				return false;
			}
			OutValue.ImportedData.MaterialSlots.resize(static_cast<size_t>(SlotCount));
			for (auto& Slot : OutValue.ImportedData.MaterialSlots)
				if (!ReadString(Bytes, Slot.Name)
					|| !ReadValue(Bytes, Slot.SourceMaterialIndex)
					|| !ReadString(Bytes, Slot.SourceName))
				{
					OutError = "StaticMesh material-slot payload is malformed.";
					return false;
				}
			if (!ReadValue(Bytes, MeshCount) || MeshCount > MaxImportedSourceMeshes)
			{
				OutError = "StaticMesh mesh count exceeds its contract.";
				return false;
			}
			OutValue.ImportedData.Meshes.resize(static_cast<size_t>(MeshCount));
			constexpr uint64 MaximumElements = 268'435'456;
			for (auto& Mesh : OutValue.ImportedData.Meshes)
			{
				if (!ReadString(Bytes, Mesh.Name)
					|| !ReadValue(Bytes, Mesh.SourceMaterialIndex)
					|| !ReadTrivialVector(Bytes, Mesh.Positions, MaximumElements)
					|| !ReadTrivialVector(Bytes, Mesh.Normals, MaximumElements)
					|| !ReadTrivialVector(Bytes, Mesh.Tangents, MaximumElements))
				{
					OutError = "StaticMesh vertex payload is malformed.";
					return false;
				}
				for (auto& UVs : Mesh.UVChannels)
					if (!ReadTrivialVector(Bytes, UVs, MaximumElements))
					{
						OutError = "StaticMesh UV payload is malformed.";
						return false;
					}
				if (!ReadTrivialVector(Bytes, Mesh.Colors, MaximumElements)
					|| !ReadTrivialVector(Bytes, Mesh.Indices, MaximumElements))
				{
					OutError = "StaticMesh color or index payload is malformed.";
					return false;
				}
			}
			if (!Bytes.empty() || OutValue.SourcePath.IsEmpty()
				|| OutValue.ImportedData.Meshes.empty())
			{
				OutError = "StaticMesh source-node payload is incomplete.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeStaticMeshInterchangePlan(const FStaticMeshInterchangePlan& Plan)
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			AppendValue(Bytes, Plan.Settings.ForwardAxis);
			AppendValue(Bytes, Plan.Settings.RightAxis);
			AppendValue(Bytes, Plan.Settings.UpAxis);
			return MakeInterchangePayload(
				std::string(StaticMeshPlanSchema), 1, std::move(Bytes));
		}

		auto DecodeStaticMeshInterchangePlan(
			const FInterchangePayload& Payload,
			FStaticMeshInterchangePlan& OutPlan,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != StaticMeshPlanSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "StaticMesh plan payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, OutPlan.Policy)
				|| !ReadValue(Bytes, OutPlan.Settings.ForwardAxis)
				|| !ReadValue(Bytes, OutPlan.Settings.RightAxis)
				|| !ReadValue(Bytes, OutPlan.Settings.UpAxis)
				|| !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, OutPlan.Destination, &OutError)
				|| !OutPlan.Settings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "StaticMesh plan payload is malformed.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeImageInterchangeValue(const FDecodedImageInterchangeValue& Value)
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Value.SourceData.Width);
			AppendValue(Bytes, Value.SourceData.Height);
			AppendValue(Bytes, Value.SourceData.SourceChannelCount);
			AppendValue(Bytes, Value.SourceData.Format);
			AppendValue(Bytes, Value.SourceData.bHasTransparency);
			AppendValue(Bytes, Value.SourceHash.HashLow);
			AppendValue(Bytes, Value.SourceHash.HashHigh);
			AppendValue(Bytes, Value.SourceFileSize);
			AppendValue(Bytes, Value.SourceLastWriteTime);
			AppendString(Bytes, Value.SourcePath.Path);
			const uint64 PixelBytes = Value.SourceData.Pixels.size();
			AppendValue(Bytes, PixelBytes);
			Bytes.insert(Bytes.end(), Value.SourceData.Pixels.begin(),
				Value.SourceData.Pixels.end());
			return MakeInterchangePayload(std::string(ImageNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeImageInterchangeValue(
			const FInterchangePayload& Payload,
			FDecodedImageInterchangeValue& OutValue,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != ImageNodeSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "Normalized image payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			uint64 PixelBytes = 0;
			if (!ReadValue(Bytes, OutValue.SourceData.Width)
				|| !ReadValue(Bytes, OutValue.SourceData.Height)
				|| !ReadValue(Bytes, OutValue.SourceData.SourceChannelCount)
				|| !ReadValue(Bytes, OutValue.SourceData.Format)
				|| !ReadValue(Bytes, OutValue.SourceData.bHasTransparency)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashLow)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashHigh)
				|| !ReadValue(Bytes, OutValue.SourceFileSize)
				|| !ReadValue(Bytes, OutValue.SourceLastWriteTime)
				|| !ReadString(Bytes, OutValue.SourcePath.Path)
				|| !ReadValue(Bytes, PixelBytes) || PixelBytes != Bytes.size())
			{
				OutError = "Normalized image payload is malformed.";
				return false;
			}
			OutValue.SourceData.Pixels.assign(Bytes.begin(), Bytes.end());
			if (!OutValue.SourceData.IsValid() || OutValue.SourcePath.IsEmpty())
			{
				OutError = "Normalized image payload contains invalid source data.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeTexture2DInterchangePlan(const FTexture2DInterchangePlan& Plan)
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			AppendValue(Bytes, Plan.Settings.Usage);
			AppendValue(Bytes, Plan.Settings.CompressionQuality);
			AppendValue(Bytes, Plan.Settings.AlphaMipMode);
			AppendValue(Bytes, Plan.Settings.AlphaCoverageThreshold);
			AppendValue(Bytes, Plan.Settings.MaxResolution);
			const int8 SRGB = !Plan.Settings.bSRGB ? -1
				: *Plan.Settings.bSRGB ? 1 : 0;
			AppendValue(Bytes, SRGB);
			return MakeInterchangePayload(std::string(Texture2DPlanSchema), 1,
				std::move(Bytes));
		}

		auto DecodeTexture2DInterchangePlan(
			const FInterchangePayload& Payload,
			FTexture2DInterchangePlan& OutPlan,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != Texture2DPlanSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "Texture2D plan payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			std::string Destination;
			int8 SRGB = -1;
			if (!ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, OutPlan.Policy)
				|| !ReadValue(Bytes, OutPlan.Settings.Usage)
				|| !ReadValue(Bytes, OutPlan.Settings.CompressionQuality)
				|| !ReadValue(Bytes, OutPlan.Settings.AlphaMipMode)
				|| !ReadValue(Bytes, OutPlan.Settings.AlphaCoverageThreshold)
				|| !ReadValue(Bytes, OutPlan.Settings.MaxResolution)
				|| !ReadValue(Bytes, SRGB) || !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, OutPlan.Destination, &OutError)
				|| (SRGB < -1 || SRGB > 1))
			{
				if (OutError.empty()) OutError = "Texture2D plan payload is malformed.";
				return false;
			}
			if (SRGB >= 0) OutPlan.Settings.bSRGB = SRGB != 0;
			OutError.clear();
			return true;
		}

		class FImageInterchangeTranslator final : public IInterchangeTranslator
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
				const FInterchangePayload& Settings,
				FTranslatedAssetGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
				FDecodedImageInterchangeValue Value;
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
					.Payload = EncodeImageInterchangeValue(Value),
					.SourceIdentities = {"root"}});
			}
		};

		class FDefaultTexture2DInterchangePipeline final : public IInterchangePipeline
		{
		public:
			auto Execute(
				const FTranslatedAssetGraph& TranslatedGraph,
				const FImportFactoryGraph*,
				const FInterchangePayload& Settings,
				FImportFactoryGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FTexture2DInterchangePlan Plan;
				std::string Error;
				if (!TranslatedGraph.FindNode("image")
					|| !DecodeTexture2DInterchangePlan(Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidPlan,
						.Identity = "Durin.Texture2D.InvalidPlan",
						.Phase = "Pipeline", .Message = std::move(Error)});
					return false;
				}
				return Builder.AddNode({
					.StableIdentity = "texture2d",
					.FactoryId = std::string(Texture2DFactoryId),
					.FactoryContractVersion = 1,
					.OutputClassName = "Durin::DTexture2D",
					.Destination = Plan.Destination,
					.Policy = Plan.Policy,
					.Settings = Settings,
					.TranslatedNodeReferences = {"image"}});
			}
		};

		class FTexture2DInterchangeProduct final : public IInterchangeFactoryProduct
		{
		public:
			Asset::Build::FTexture2DBuildProduct Product;
			Asset::Build::FTexture2DPublicationContext Publication;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				return std::make_unique<FTexture2DInterchangeProduct>(*this);
			}
		};

		class FTexture2DInterchangeFactory final : public IInterchangeFactory
		{
		public:
			auto BuildDetachedProduct(
				const FImportFactoryNode& FactoryNode,
				const FTranslatedAssetGraph& TranslatedGraph,
				IImportProgressReporter*,
				const std::function<bool()>& IsCancellationRequested,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				const FTranslatedAssetNode* Image = TranslatedGraph.FindNode("image");
				FDecodedImageInterchangeValue Source;
				FTexture2DInterchangePlan Plan;
				std::string Error;
				if (!Image || !DecodeImageInterchangeValue(Image->Payload, Source, Error)
					|| !DecodeTexture2DInterchangePlan(FactoryNode.Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.Texture2D.PayloadInvalid",
						.Phase = "ProductBuild", .Message = std::move(Error)});
					return {};
				}
				auto Result = std::make_unique<FTexture2DInterchangeProduct>();
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
				const FImportFactoryNode& FactoryNode,
				std::unique_ptr<IInterchangeFactoryProduct> Product,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* TextureProduct = dynamic_cast<FTexture2DInterchangeProduct*>(Product.get());
				FAssetPath CandidatePath = FactoryNode.Destination;
				if (FactoryNode.Policy != EImportOutputPolicy::Create
					&& !MakeCandidatePath(FactoryNode.Destination, CandidatePath)) return {};
				DTexture2D* Candidate = nullptr;
				if (!TextureProduct || !Asset::CreateAsset(CandidatePath, Candidate)) return {};
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(
					Candidate, FactoryNode.Policy == EImportOutputPolicy::Create);
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
				const FInterchangeProvenance& Provenance,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				auto* Texture = Cast<DTexture2D>(&AssetObject);
				std::vector<std::byte> Bytes;
				std::string Error;
				if (!Texture || !SerializeInterchangeProvenance(Provenance, Bytes, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::PublicationFailure,
						.Identity = "Durin.Texture2D.ProvenanceFailed",
						.Phase = "Publication", .Message = std::move(Error)});
					return false;
				}
				Texture->PublishInterchangeProvenance(std::move(Bytes));
				return true;
			}
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

		class FGeometryInterchangeTranslator final : public IInterchangeTranslator
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
				const FInterchangePayload& Settings,
				FTranslatedAssetGraphBuilder& Builder,
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
				FDecodedStaticMeshInterchangeValue Value{
					.ImportedData = MakeStaticMeshImportedData(Scene),
					.SourcePath = Root->SourcePath,
					.SourceHash = Root->ContentHash};
				return Builder.AddNode({
					.StableIdentity = "mesh:combined",
					.NodeKind = "Durin.Geometry.StaticMesh",
					.Payload = EncodeStaticMeshInterchangeValue(Value),
					.SourceIdentities = {"root"}});
			}
		};

		class FDefaultStaticMeshInterchangePipeline final : public IInterchangePipeline
		{
		public:
			auto Execute(
				const FTranslatedAssetGraph& TranslatedGraph,
				const FImportFactoryGraph*,
				const FInterchangePayload& Settings,
				FImportFactoryGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FStaticMeshInterchangePlan Plan;
				std::string Error;
				if (!TranslatedGraph.FindNode("mesh:combined")
					|| !DecodeStaticMeshInterchangePlan(Settings, Plan, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidPlan,
						.Identity = "Durin.StaticMesh.InvalidPlan",
						.Phase = "Pipeline", .Message = std::move(Error)});
					return false;
				}
				return Builder.AddNode({
					.StableIdentity = "static-mesh",
					.FactoryId = std::string(StaticMeshFactoryId),
					.FactoryContractVersion = 1,
					.OutputClassName = "Durin::DStaticMesh",
					.Destination = Plan.Destination,
					.Policy = Plan.Policy,
					.Settings = Settings,
					.TranslatedNodeReferences = {"mesh:combined"}});
			}
		};

		auto CloneStaticMeshBuildProduct(
			const Asset::Build::FStaticMeshBuildProduct& Source)
			-> Asset::Build::FStaticMeshBuildProduct
		{
			Asset::Build::FStaticMeshBuildProduct Result;
			if (Source.RenderData)
				Result.RenderData = std::make_unique<FStaticMeshRenderData>(*Source.RenderData);
			Result.MaterialSlots = Source.MaterialSlots;
			Result.SourceImportData = Source.SourceImportData;
			Result.DerivedDataKey = Source.DerivedDataKey;
			Result.bSlotMetadataChanged = Source.bSlotMetadataChanged;
			Result.DerivedDataStatus = Source.DerivedDataStatus;
			Result.DiagnosticMessage = Source.DiagnosticMessage;
			Result.bSourceImporterInvoked = Source.bSourceImporterInvoked;
			Result.bMarkPackageDirty = Source.bMarkPackageDirty;
			Result.FailureStage = Source.FailureStage;
			return Result;
		}

		class FStaticMeshInterchangeProduct final : public IInterchangeFactoryProduct
		{
		public:
			Asset::Build::FStaticMeshBuildProduct Product;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				auto Result = std::make_unique<FStaticMeshInterchangeProduct>();
				Result->Product = CloneStaticMeshBuildProduct(Product);
				return Result;
			}
		};

		class FStaticMeshInterchangeFactory final : public IInterchangeFactory
		{
		public:
			auto BuildDetachedProduct(
				const FImportFactoryNode& FactoryNode,
				const FTranslatedAssetGraph& TranslatedGraph,
				IImportProgressReporter*,
				const std::function<bool()>& IsCancellationRequested,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				if (IsCancellationRequested()) return {};
				const FTranslatedAssetNode* MeshNode =
					TranslatedGraph.FindNode("mesh:combined");
				FDecodedStaticMeshInterchangeValue Source;
				FStaticMeshInterchangePlan Plan;
				std::string Error;
				if (!MeshNode
					|| !DecodeStaticMeshInterchangeValue(MeshNode->Payload, Source, Error)
					|| !DecodeStaticMeshInterchangePlan(FactoryNode.Settings, Plan, Error))
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
				Asset::Build::FStaticMeshReconciliationSnapshot Reconciliation{
					.StableObjectPath = Plan.Destination.ToString(),
					.Provenance = Provenance,
					.ImportSettings = Plan.Settings};
				auto Result = std::make_unique<FStaticMeshInterchangeProduct>();
				if (!Asset::Build::FStaticMeshBuildOperations::BuildImportedProduct(
					Reconciliation, Source.ImportedData, std::move(Provenance),
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

			auto MaterializeCandidate(
				const FImportFactoryNode& FactoryNode,
				std::unique_ptr<IInterchangeFactoryProduct> Product,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* MeshProduct = dynamic_cast<FStaticMeshInterchangeProduct*>(Product.get());
				FAssetPath CandidatePath = FactoryNode.Destination;
				if (FactoryNode.Policy != EImportOutputPolicy::Create
					&& !MakeCandidatePath(FactoryNode.Destination, CandidatePath)) return {};
				DStaticMesh* Candidate = nullptr;
				if (!MeshProduct || !Asset::CreateAsset(CandidatePath, Candidate)) return {};
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(
					Candidate, FactoryNode.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::FStaticMeshBuildOperations::PublishImportedProduct(
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

			auto ApplyProvenance(
				DObject& AssetObject,
				const FInterchangeProvenance& Provenance,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				std::vector<std::byte> Bytes;
				std::string Error;
				if (!Mesh || !SerializeInterchangeProvenance(Provenance, Bytes, Error))
				{
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::PublicationFailure,
						.Identity = "Durin.StaticMesh.ProvenanceFailed",
						.Phase = "Publication", .Message = std::move(Error)});
					return false;
				}
				Mesh->PublishInterchangeProvenance(std::move(Bytes));
				return true;
			}
		};

		class FSceneInterchangeTranslator final : public IInterchangeTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				std::string Extension(Source.Extension);
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Value) {
					return static_cast<char>(std::tolower(Value)); });
				return Extension == ".gltf" || Extension == ".glb" || Extension == ".fbx"
					|| Extension == ".obj" || Extension == ".dae" || Extension == ".3ds"
					|| Extension == ".ply" || Extension == ".stl";
			}
			auto DiscoverDependencies(
				std::span<const FSourceSnapshotEntry> Sources,
				FDependencyRequestSink& Sink,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				return DiscoverSceneInterchangeDependencies(Sources, Sink, OutDiagnostics);
			}
			auto Translate(
				const FSourceSnapshot& Snapshot,
				const FInterchangePayload& Settings,
				FTranslatedAssetGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FSceneInterchangePlan Request;
				std::string Error;
				if (!DecodeSceneInterchangePlan(Settings, Request, Error))
				{
					OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidPlan,
						.Identity = "Durin.Scene.InvalidTranslationSettings",
						.Phase = "Translation", .Message = std::move(Error)});
					return false;
				}
				std::shared_ptr<const FSceneProviderPlanData> Data;
				std::vector<FImportOutputPreview> Outputs;
				if (!BuildSceneInterchangePlanData(Snapshot, Request.DestinationDirectory,
					Request.MeshSettings, Data, Outputs, OutDiagnostics, Error))
				{
					OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ProviderFailure,
						.Identity = "Durin.Scene.DecodeFailed", .Phase = "Translation",
						.SourceIdentity = "root", .Message = std::move(Error)});
					return false;
				}
				auto MutableData = std::make_shared<FSceneProviderPlanData>(*Data);
				MutableData->Outputs.push_back({.StableIdentity = "scene-import-record",
					.Kind = ESceneOutputKind::ImportRecord});
				const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
				const std::string RecordName = Root
					? std::filesystem::path(Root->SourcePath.Path).stem().generic_string() + "_Import"
					: "Scene_Import";
				FAssetPath RecordPath;
				if (!FAssetPath::TryCreate(std::format("{}/{}",
					Request.DestinationDirectory.ToString(), RecordName), RecordPath, &Error))
					return false;
				Outputs.push_back({.StableIdentity = "scene-import-record", .Role = "ImportRecord",
					.AssetPath = RecordPath, .AssetClassName = "Durin::Asset::DImportRecord",
					.Policy = EImportOutputPolicy::Create, .Collision = EImportCollisionAction::Create});
				Data = std::move(MutableData);
				std::vector<std::byte> KeyBytes;
				AppendValue(KeyBytes, Request.MeshSettings.ForwardAxis);
				AppendValue(KeyBytes, Request.MeshSettings.RightAxis);
				AppendValue(KeyBytes, Request.MeshSettings.UpAxis);
				for (const FSourceSnapshotEntry& Source : Snapshot.GetSources())
				{
					AppendString(KeyBytes, Source.StableIdentity);
					AppendValue(KeyBytes, Source.ContentHash.HashLow);
					AppendValue(KeyBytes, Source.ContentHash.HashHigh);
				}
				const std::string CacheKey = FXxHash128::HashBuffer(KeyBytes).ToString();
				auto Cached = std::make_shared<FSceneInterchangeCachedPlan>();
				Cached->Data = std::move(Data);
				Cached->Snapshot = std::make_shared<FSourceSnapshot>(Snapshot);
				Cached->Outputs = std::move(Outputs);
				{
					std::lock_guard Lock(GSceneInterchangeCacheMutex);
					GSceneInterchangeCache[CacheKey] = Cached;
					GSceneInterchangeOutputCache[
						MakeSceneOutputCacheKey(CacheKey, Request.DestinationDirectory)] = Cached->Outputs;
				}
				std::unordered_map<uint32, std::string> MaterialIdentities;
				for (const FSceneOutputData& Output : Cached->Data->Outputs)
					if (Output.Kind == ESceneOutputKind::MaterialInstance)
						MaterialIdentities.emplace(Output.SourceIndex, Output.StableIdentity);
				for (uint32 Index = 0; Index < Cached->Data->Outputs.size(); ++Index)
				{
					const FSceneOutputData& Output = Cached->Data->Outputs[Index];
					std::vector<std::string> Dependencies;
					if (Output.Kind == ESceneOutputKind::MaterialInstance)
						for (const FSceneMaterialTextureBinding& Binding : Output.TextureBindings)
							Dependencies.push_back(Binding.TextureIdentity);
					else if (Output.Kind == ESceneOutputKind::SkeletalMesh
						|| Output.Kind == ESceneOutputKind::AnimationClip)
						Dependencies.push_back(Output.SkeletonIdentity);
					if (Output.Kind == ESceneOutputKind::StaticMesh
						|| Output.Kind == ESceneOutputKind::SkeletalMesh)
						for (const auto& [_, Identity] : MaterialIdentities)
							Dependencies.push_back(Identity);
					if (Output.Kind == ESceneOutputKind::ImportRecord)
						for (const FSceneOutputData& Dependency : Cached->Data->Outputs)
							if (Dependency.Kind != ESceneOutputKind::ImportRecord)
								Dependencies.push_back(Dependency.StableIdentity);
					std::vector<std::string> SourceIdentities{"root"};
					if (Output.Kind == ESceneOutputKind::Texture2D
						&& Output.SourceIndex < Cached->Data->Scene.Images.size())
					{
						const FImportedImage& Image = Cached->Data->Scene.Images[Output.SourceIndex];
						if (Image.ExternalDependencyIndex
							&& *Image.ExternalDependencyIndex < Cached->Data->Scene.Dependencies.size())
							SourceIdentities.push_back(Cached->Data->Scene.Dependencies[
								*Image.ExternalDependencyIndex].StableIdentity);
					}
					if (!Builder.AddNode({.StableIdentity = Output.StableIdentity,
						.NodeKind = std::string(SceneNodeKind(Output.Kind)),
						.Payload = EncodeSceneNodeReference(CacheKey, Index),
						.SourceIdentities = std::move(SourceIdentities),
						.Dependencies = std::move(Dependencies)})) return false;
				}
				return true;
			}
		};

		class FDefaultSceneInterchangePipeline final : public IInterchangePipeline
		{
		public:
			auto Execute(
				const FTranslatedAssetGraph& TranslatedGraph,
				const FImportFactoryGraph*,
				const FInterchangePayload& Settings,
				FImportFactoryGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FSceneInterchangePlan Request;
				std::string Error;
				if (!DecodeSceneInterchangePlan(Settings, Request, Error)) return false;
				for (const FTranslatedAssetNode& Node : TranslatedGraph.GetNodes())
				{
					std::shared_ptr<const FSceneInterchangeCachedPlan> Cached;
					const FSceneOutputData* Output = nullptr;
					if (!DecodeSceneNodeReference(Node.Payload, Cached, Output, Error))
					{
						OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::InvalidPlan,
							.Identity = "Durin.Scene.StaleNode", .Phase = "Pipeline",
							.OutputIdentity = Node.StableIdentity, .Message = std::move(Error)});
						return false;
					}
					std::vector<FImportOutputPreview> Outputs;
					std::span<const std::byte> ReferenceBytes(Node.Payload.Bytes);
					std::string SourceKey;
					uint32 IgnoredIndex = 0;
					if (!ReadString(ReferenceBytes, SourceKey) || !ReadValue(ReferenceBytes, IgnoredIndex))
						return false;
					{
						std::lock_guard Lock(GSceneInterchangeCacheMutex);
						const auto It = GSceneInterchangeOutputCache.find(
							MakeSceneOutputCacheKey(SourceKey, Request.DestinationDirectory));
						if (It == GSceneInterchangeOutputCache.end()) return false;
						Outputs = It->second;
					}
					const auto Preview = std::ranges::find(
						Outputs, Output->StableIdentity, &FImportOutputPreview::StableIdentity);
					if (Preview == Outputs.end()) return false;
					const auto Existing = std::ranges::find(Request.ExistingMappings,
						Output->StableIdentity, &FInterchangeOutputMapping::OutputIdentity);
					const FAssetPath OutputPath = Existing == Request.ExistingMappings.end()
						? Preview->AssetPath : Existing->AssetPath;
					Preview->AssetPath = OutputPath;
					Preview->Policy = Existing == Request.ExistingMappings.end()
						? Preview->Policy : EImportOutputPolicy::ReplaceWholeState;
					{
						std::lock_guard Lock(GSceneInterchangeCacheMutex);
						GSceneInterchangeOutputCache[
							MakeSceneOutputCacheKey(SourceKey, Request.DestinationDirectory)] = Outputs;
					}
					if (!Builder.AddNode({.StableIdentity = Output->StableIdentity,
						.FactoryId = std::string(SceneFactoryIds[SceneFactoryIndex(Output->Kind)]),
						.FactoryContractVersion = 1,
						.OutputClassName = std::string(SceneOutputClassName(Output->Kind)),
						.Destination = OutputPath,
						.Policy = Existing == Request.ExistingMappings.end()
							? Preview->Policy : EImportOutputPolicy::ReplaceWholeState,
						.Settings = Settings,
						.TranslatedNodeReferences = {Output->StableIdentity},
						.FactoryDependencies = Node.Dependencies})) return false;
				}
				for (const FInterchangeOutputMapping& Existing : Request.ExistingMappings)
					if (!TranslatedGraph.FindNode(Existing.OutputIdentity))
						OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Warning,
							.Category = EImportDiagnosticCategory::StalePlan,
							.Identity = "Durin.Scene.RemovedOutput", .Phase = "Pipeline",
							.OutputIdentity = Existing.OutputIdentity,
							.Message = "Previously managed Scene output is absent and remains orphaned."});
				return true;
			}
		};

		class FSceneInterchangeFactoryProduct final : public IInterchangeFactoryProduct
		{
		public:
			std::shared_ptr<const FSceneInterchangeCachedPlan> Cached;
			uint32 OutputIndex = 0;
			std::vector<FImportOutputPreview> Outputs;
			Asset::Build::FStaticMeshBuildProduct StaticMesh;
			FSceneInterchangeTextureProduct Texture;
			Asset::Build::FSkeletalMeshBuildProduct SkeletalMesh;
			Asset::Build::FAnimationClipBuildProduct Animation;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				auto Result = std::make_unique<FSceneInterchangeFactoryProduct>();
				Result->Cached = Cached;
				Result->OutputIndex = OutputIndex;
				Result->Outputs = Outputs;
				Result->StaticMesh = CloneStaticMeshBuildProduct(StaticMesh);
				Result->Texture = Texture;
				Result->SkeletalMesh = SkeletalMesh;
				Result->Animation = Animation;
				return Result;
			}
		};

		template<typename TExchange>
		class TOwnedSceneExchange final : public IPreparedImportedStateExchange
		{
		public:
			explicit TOwnedSceneExchange(std::unique_ptr<TExchange> InExchange)
				: Exchange(std::move(InExchange)) {}
			auto Commit() noexcept -> void override { Exchange->Commit(); }
			auto Reverse() noexcept -> void override { Exchange->Reverse(); }
			auto Finalize() noexcept -> void override { Exchange->Finalize(); }
		private:
			std::unique_ptr<TExchange> Exchange;
		};

			class FSceneInterchangeFactory final : public IInterchangeFactory
		{
			public:
				explicit FSceneInterchangeFactory(ESceneOutputKind InKind) : Kind(InKind) {}

				auto LoadExistingTarget(
					const FImportFactoryNode& FactoryNode,
					DObject*& OutTarget) const -> Asset::FAssetResult override
				{
					const FScopedSkeletalDerivedDataRepairLoad RepairLoad;
					return Asset::LoadAsset(FactoryNode.Destination, OutTarget);
				}

			auto BuildDetachedProduct(
				const FImportFactoryNode& FactoryNode,
				const FTranslatedAssetGraph& TranslatedGraph,
				IImportProgressReporter*,
				const std::function<bool()>& IsCancellationRequested,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				if (IsCancellationRequested()) return {};
				const FTranslatedAssetNode* Node = FactoryNode.TranslatedNodeReferences.empty()
					? nullptr : TranslatedGraph.FindNode(FactoryNode.TranslatedNodeReferences.front());
				std::shared_ptr<const FSceneInterchangeCachedPlan> Cached;
				const FSceneOutputData* Output = nullptr;
				std::string Error;
				if (!Node || !DecodeSceneNodeReference(Node->Payload, Cached, Output, Error)
					|| Output->Kind != Kind)
				{
					OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.Scene.FactoryPayloadInvalid", .Phase = "ProductBuild",
						.OutputIdentity = FactoryNode.StableIdentity, .Message = std::move(Error)});
					return {};
				}
				auto Product = std::make_unique<FSceneInterchangeFactoryProduct>();
				Product->Cached = Cached;
				Product->OutputIndex = static_cast<uint32>(Output - Cached->Data->Outputs.data());
				FSceneInterchangePlan FactoryPlan;
				if (!DecodeSceneInterchangePlan(FactoryNode.Settings, FactoryPlan, Error))
					return Fail(FactoryNode, std::move(Error), OutDiagnostics);
				const FInterchangePayload AuthoredSettings =
					EncodeSceneAuthoredSettings(FactoryPlan);
				std::span<const std::byte> ReferenceBytes(Node->Payload.Bytes);
				std::string SourceKey;
				uint32 IgnoredIndex = 0;
				if (!ReadString(ReferenceBytes, SourceKey) || !ReadValue(ReferenceBytes, IgnoredIndex))
					return Fail(FactoryNode, "Scene source-node reference is malformed.", OutDiagnostics);
				{
					std::lock_guard Lock(GSceneInterchangeCacheMutex);
					const auto It = GSceneInterchangeOutputCache.find(
						MakeSceneOutputCacheKey(SourceKey, FactoryPlan.DestinationDirectory));
					if (It == GSceneInterchangeOutputCache.end())
						return Fail(FactoryNode, "Scene output plan is unavailable.", OutDiagnostics);
					Product->Outputs = It->second;
				}
				const FSourceSnapshotEntry* Root = Cached->Snapshot->FindSource("root");
				if (Kind == ESceneOutputKind::Texture2D)
				{
					if (!BuildSceneInterchangeTextureProduct(*Cached->Snapshot, *Cached->Data,
						*Output, IsCancellationRequested, Product->Texture, Error)) return Fail(
						FactoryNode, std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::StaticMesh)
				{
					if (!Root) return Fail(FactoryNode, "Scene root source is unavailable.", OutDiagnostics);
					FStaticMeshSourceImportData Provenance{
						.SourcePath = Root->SourcePath,
						.SourceContentHash = Root->ContentHash.ToString(),
						.ImporterId = std::string(SceneTranslatorId), .ImporterVersion = 1,
						.ImportSettings = Cached->Data->MeshSettings};
					Asset::Build::FStaticMeshReconciliationSnapshot Reconciliation{
						.StableObjectPath = FactoryNode.Destination.ToString(),
						.Provenance = Provenance, .ImportSettings = Cached->Data->MeshSettings};
					if (!Asset::Build::FStaticMeshBuildOperations::BuildImportedProduct(
						Reconciliation, MakeStaticMeshImportedData(Cached->Data->Scene),
						std::move(Provenance), Root->SourcePath.Path, Product->StaticMesh, Error))
						return Fail(FactoryNode, std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::SkeletalMesh)
				{
					if (Output->SourceIndex >= Cached->Data->Scene.SkeletalMeshes.size())
						return Fail(FactoryNode, "Scene SkeletalMesh mapping is invalid.", OutDiagnostics);
					const FImportedSkeletalMeshData& Imported =
						Cached->Data->Scene.SkeletalMeshes[Output->SourceIndex];
					if (Imported.SkeletonIndex >= Cached->Data->Scene.Skeletons.size())
						return Fail(FactoryNode, "Scene Skeleton mapping is invalid.", OutDiagnostics);
					const FImportedSkeletonData& Skeleton = Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					Asset::Build::FSkeletalMeshBuildKeyInput Key;
					static_cast<Asset::Build::FSkeletalBuildKeyFields&>(Key) = {
						.ProviderIdentity = std::string(SceneTranslatorId), .ProviderVersion = 1,
						.SourceClosureHash = Root ? Root->ContentHash : FXxHash128{},
						.SettingsHash = AuthoredSettings.ContentHash,
						.ProviderStateHash = Node->Payload.ContentHash,
						.StableOutputIdentity = Output->StableIdentity,
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
						.TargetProfile = ESkeletalPayloadTargetProfile::Game};
					if (!Asset::Build::BuildSkeletalMeshProduct({
						.SkeletonBoneCount = static_cast<uint32>(Skeleton.Bones.size()),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.MeshNodeBindTransform = Imported.MeshNodeBindTransform,
						.MaterialSlotCount = static_cast<uint32>(Imported.MaterialSlots.size()),
						.Payload = Imported.Payload, .KeyInput = std::move(Key)},
						Product->SkeletalMesh, Error))
						return Fail(FactoryNode, std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::AnimationClip)
				{
					if (Output->SourceIndex >= Cached->Data->Scene.AnimationClips.size())
						return Fail(FactoryNode, "Scene AnimationClip mapping is invalid.", OutDiagnostics);
					const FImportedAnimationClipData& Imported =
						Cached->Data->Scene.AnimationClips[Output->SourceIndex];
					if (Imported.SkeletonIndex >= Cached->Data->Scene.Skeletons.size())
						return Fail(FactoryNode, "Scene Skeleton mapping is invalid.", OutDiagnostics);
					const FImportedSkeletonData& Skeleton = Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					Asset::Build::FAnimationClipBuildKeyInput Key;
					static_cast<Asset::Build::FSkeletalBuildKeyFields&>(Key) = {
						.ProviderIdentity = std::string(SceneTranslatorId), .ProviderVersion = 1,
						.SourceClosureHash = Root ? Root->ContentHash : FXxHash128{},
						.SettingsHash = AuthoredSettings.ContentHash,
						.ProviderStateHash = Node->Payload.ContentHash,
						.StableOutputIdentity = Output->StableIdentity,
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
						.TargetProfile = ESkeletalPayloadTargetProfile::Game};
					if (!Asset::Build::BuildAnimationClipProduct({
						.SkeletonBoneCount = static_cast<uint32>(Skeleton.Bones.size()),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.ClipName = FName(Imported.SuggestedName), .Payload = Imported.Payload,
						.KeyInput = std::move(Key)}, Product->Animation, Error))
						return Fail(FactoryNode, std::move(Error), OutDiagnostics);
				}
				return Product;
			}

			auto MaterializeCandidate(
				const FImportFactoryNode& FactoryNode,
				std::unique_ptr<IInterchangeFactoryProduct> InProduct,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto Product = std::unique_ptr<FSceneInterchangeFactoryProduct>(
					dynamic_cast<FSceneInterchangeFactoryProduct*>(InProduct.release()));
				if (!Product || Product->OutputIndex >= Product->Cached->Data->Outputs.size()) return {};
				FAssetPath CandidatePath = FactoryNode.Destination;
				if (FactoryNode.Policy != EImportOutputPolicy::Create
					&& !MakeCandidatePath(FactoryNode.Destination, CandidatePath)) return {};
				DObject* Candidate = nullptr;
				Asset::FAssetResult Created;
				if (Kind == ESceneOutputKind::StaticMesh) { DStaticMesh* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::MaterialInstance) { DMaterialInstance* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::Skeleton) { DSkeleton* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::SkeletalMesh) { DSkeletalMesh* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::AnimationClip) { DAnimationClip* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::Texture2D) { DTexture2D* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else { Asset::DImportRecord* Value = nullptr; Created = Asset::CreateImportRecordAsset(CandidatePath, Value); Candidate = Value; }
				if (!Created || !Candidate) return {};
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(
					Candidate, FactoryNode.Policy == EImportOutputPolicy::Create);
				std::string Error;
				const FSceneOutputData& Output = Product->Cached->Data->Outputs[Product->OutputIndex];
				if (Kind == ESceneOutputKind::Skeleton)
				{
					if (Output.SourceIndex >= Product->Cached->Data->Scene.Skeletons.size()
						|| !Cast<DSkeleton>(Candidate)->InitializeCanonicalBones(
							Product->Cached->Data->Scene.Skeletons[Output.SourceIndex].Bones, Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::Texture2D)
				{
					if (!Asset::Build::PublishTexture2DProduct(*Cast<DTexture2D>(Candidate),
						std::move(Product->Texture.Product), {.SourcePath = Product->Texture.Source,
							.DecoderId = "DurinImage", .DecoderVersion = 1,
							.SourceFileSize = Product->Texture.SourceFileSize}, Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::StaticMesh)
				{
					if (!Asset::Build::FStaticMeshBuildOperations::PublishImportedProduct(
						*Cast<DStaticMesh>(Candidate), std::move(Product->StaticMesh), Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::ImportRecord)
				{
					FSceneInterchangePlan PersistedPlan;
					if (!DecodeSceneInterchangePlan(
						FactoryNode.Settings, PersistedPlan, Error))
						return MaterializationFailure(
							std::move(Result), std::move(Error), OutDiagnostics);
					const FInterchangePayload AuthoredSettings =
						EncodeSceneAuthoredSettings(PersistedPlan);
					Asset::FImportRecordState State{
						.ProviderId = std::string(SceneTranslatorId), .ProviderContractVersion = 1};
					if (!Asset::MakeImportRecordPayload(std::string(ScenePlanSchema), 1,
						AuthoredSettings.Bytes, Asset::MaximumImportRecordSettingsBytes,
						State.Settings, Error)
						|| !Asset::MakeImportRecordPayload("Durin.Scene.InterchangeState", 1,
							AuthoredSettings.Bytes, Asset::MaximumImportRecordProviderStateBytes,
							State.ProviderState, Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
					for (const FSourceSnapshotEntry& Source : Product->Cached->Snapshot->GetSources())
						State.Sources.push_back({.StableIdentity = Source.StableIdentity,
							.Role = Source.Role, .SourcePath = Source.SourcePath,
							.ContentHashLow = Source.ContentHash.HashLow,
							.ContentHashHigh = Source.ContentHash.HashHigh,
							.ByteCount = Source.ByteCount});
					for (const FImportOutputPreview& Preview : Product->Outputs)
					{
						if (Preview.StableIdentity == "scene-import-record") continue;
						std::vector<std::byte> FingerprintBytes;
						AppendString(FingerprintBytes, Preview.StableIdentity);
						AppendValue(FingerprintBytes, AuthoredSettings.ContentHash.HashLow);
						AppendValue(FingerprintBytes, AuthoredSettings.ContentHash.HashHigh);
						State.Outputs.push_back({.StableIdentity = Preview.StableIdentity,
							.Role = Preview.Role, .AssetPath = Preview.AssetPath,
							.AssetClassName = Preview.AssetClassName,
							.Policy = Asset::EImportRecordOutputPolicy::Managed,
							.AuthoredFingerprint = FXxHash128::HashBuffer(FingerprintBytes).ToString()});
					}
					if (!Cast<Asset::DImportRecord>(Candidate)->SetState(std::move(State), Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				if (Kind == ESceneOutputKind::MaterialInstance
					|| Kind == ESceneOutputKind::StaticMesh
					|| Kind == ESceneOutputKind::SkeletalMesh
					|| Kind == ESceneOutputKind::AnimationClip)
				{
					std::lock_guard Lock(PendingMutex);
					Pending.emplace(Candidate, std::move(Product));
				}
				return Result;
			}

			auto ResolveCandidateDependencies(
				const FImportFactoryNode& FactoryNode,
				ISingleAssetCandidate& Candidate,
				const FInterchangeMaterializationContext& Context,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				if (Kind == ESceneOutputKind::Skeleton || Kind == ESceneOutputKind::Texture2D
					|| Kind == ESceneOutputKind::ImportRecord)
					return true;
				std::unique_ptr<FSceneInterchangeFactoryProduct> Product;
				{
					std::lock_guard Lock(PendingMutex);
					auto It = Pending.find(Candidate.GetAsset());
					if (It == Pending.end()) return false;
					Product = std::move(It->second);
					Pending.erase(It);
				}
				const FSceneOutputData& Output = Product->Cached->Data->Outputs[Product->OutputIndex];
				std::string Error;
				auto FindMaterial = [&](uint32 SourceIndex) -> DMaterialInterface* {
					for (const FSceneOutputData& Descriptor : Product->Cached->Data->Outputs)
						if (Descriptor.Kind == ESceneOutputKind::MaterialInstance
							&& Descriptor.SourceIndex == SourceIndex)
							return Cast<DMaterialInterface>(Context.ExistingTarget(Descriptor.StableIdentity));
					return nullptr;
				};
				if (Kind == ESceneOutputKind::MaterialInstance)
				{
					DMaterial* Standard = nullptr;
					FAssetPath StandardPath;
					const auto Imported = std::ranges::find(Product->Cached->Data->Scene.Materials,
						Output.SourceIndex, &FImportedMaterial::SourceMaterialIndex);
					if (!FAssetPath::TryCreate(ImportedSurfaceMaterialPath, StandardPath, &Error)
						|| !Asset::LoadAsset(StandardPath, Standard) || !Standard
						|| Imported == Product->Cached->Data->Scene.Materials.end()) return false;
					auto* Material = Cast<DMaterialInstance>(Candidate.GetAsset());
					FMaterialStaticProperties StaticProperties = Standard->GetStaticProperties();
					StaticProperties.BlendMode = Imported->AlphaMode == EImportedAlphaMode::Mask
						? EMaterialBlendMode::Masked : Imported->AlphaMode == EImportedAlphaMode::Blend
							? EMaterialBlendMode::Translucent : EMaterialBlendMode::Opaque;
					StaticProperties.bTwoSided = Imported->bDoubleSided;
					StaticProperties.OpacityMaskThreshold = Imported->AlphaCutoff;
					if (!Material || !Material->SetParent(Standard)
						|| !Material->SetStaticPropertiesOverride(StaticProperties)
						|| !Material->SetVectorParameterValue(MaterialParameters::BaseColorName(), FVector3(Imported->BaseColorFactor))
						|| !Material->SetScalarParameterValue(MaterialParameters::OpacityName(), Imported->BaseColorFactor.a)
						|| !Material->SetScalarParameterValue(MaterialParameters::MetallicName(), Imported->MetallicFactor)
						|| !Material->SetScalarParameterValue(MaterialParameters::RoughnessName(), Imported->RoughnessFactor)) return false;
					const std::array<const FName*, 8> Names{&MaterialParameters::BaseColorTextureName(),
						&MaterialParameters::NormalTextureName(), &MaterialParameters::MetallicTextureName(),
						&MaterialParameters::RoughnessTextureName(), &MaterialParameters::AmbientOcclusionTextureName(),
						&MaterialParameters::EmissiveTextureName(), &MaterialParameters::OpacityTextureName(),
						&MaterialParameters::OpacityMaskTextureName()};
					for (const FSceneMaterialTextureBinding& Binding : Output.TextureBindings)
						if (!Material->SetTextureParameterValue(*Names[Binding.MaterialRole],
							Cast<DTexture2D>(Context.ExistingTarget(Binding.TextureIdentity)))) return false;
				}
				else if (Kind == ESceneOutputKind::StaticMesh)
				{
					auto* Mesh = Cast<DStaticMesh>(Candidate.GetAsset());
					for (const FSceneOutputData& Descriptor : Product->Cached->Data->Outputs)
						if (Descriptor.Kind == ESceneOutputKind::MaterialInstance
							&& !Mesh->SetImportedDefaultMaterial(Descriptor.SourceIndex,
								Cast<DMaterialInstance>(Context.ExistingTarget(Descriptor.StableIdentity)), Error))
							return false;
				}
				else if (Kind == ESceneOutputKind::SkeletalMesh)
				{
					const FImportedSkeletalMeshData& Imported = Product->Cached->Data->Scene.SkeletalMeshes[Output.SourceIndex];
					const FImportedSkeletonData& Skeleton = Product->Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					std::vector<FMeshMaterialSlotDefinition> Slots = Imported.MaterialSlots;
					for (auto& Slot : Slots) if (!(Slot.DefaultMaterial = FindMaterial(Slot.SourceMaterialIndex))) return false;
					if (!Cast<DSkeletalMesh>(Candidate.GetAsset())->PublishBuiltProduct({
						.Skeleton = Cast<DSkeleton>(Context.ExistingTarget(Output.SkeletonIdentity)),
						.ValidationSkeleton = Cast<DSkeleton>(Context.ProspectiveObject(Output.SkeletonIdentity)),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.MeshNodeBindTransform = Product->SkeletalMesh.MeshNodeBindTransform,
						.MaterialSlots = std::move(Slots), .Payload = std::move(Product->SkeletalMesh.Payload),
						.DerivedDataKey = std::move(Product->SkeletalMesh.DerivedDataKey),
						.DiagnosticMessage = std::move(Product->SkeletalMesh.Diagnostic)}, Error)) return false;
				}
				else if (Kind == ESceneOutputKind::AnimationClip)
				{
					const FImportedAnimationClipData& Imported = Product->Cached->Data->Scene.AnimationClips[Output.SourceIndex];
					const FImportedSkeletonData& Skeleton = Product->Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					if (!Cast<DAnimationClip>(Candidate.GetAsset())->PublishBuiltProduct({
						.Skeleton = Cast<DSkeleton>(Context.ExistingTarget(Output.SkeletonIdentity)),
						.ValidationSkeleton = Cast<DSkeleton>(Context.ProspectiveObject(Output.SkeletonIdentity)),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.ClipName = Product->Animation.ClipName, .Payload = std::move(Product->Animation.Payload),
						.DerivedDataKey = std::move(Product->Animation.DerivedDataKey),
						.DiagnosticMessage = std::move(Product->Animation.Diagnostic)}, Error)) return false;
				}
				return true;
			}

			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				std::string Error;
				if (Kind == ESceneOutputKind::StaticMesh)
				{
					auto Exchange = Cast<DStaticMesh>(&Target)->PrepareImportedStateExchange(
						*Cast<DStaticMesh>(Candidate.GetAsset()), Error);
					return Exchange ? std::make_unique<FStaticMeshExchange>(std::move(Exchange)) : nullptr;
				}
				if (Kind == ESceneOutputKind::Texture2D) return std::make_unique<TNoFailExchange<DTexture2D>>(
					*Cast<DTexture2D>(&Target), *Cast<DTexture2D>(Candidate.GetAsset()));
				if (Kind == ESceneOutputKind::MaterialInstance) return std::make_unique<TNoFailExchange<DMaterialInstance>>(
					*Cast<DMaterialInstance>(&Target), *Cast<DMaterialInstance>(Candidate.GetAsset()));
				if (Kind == ESceneOutputKind::ImportRecord) return std::make_unique<TNoFailExchange<Asset::DImportRecord>>(
					*Cast<Asset::DImportRecord>(&Target), *Cast<Asset::DImportRecord>(Candidate.GetAsset()));
				if (Kind == ESceneOutputKind::Skeleton)
				{
					auto Exchange = Cast<DSkeleton>(&Target)->PrepareImportedStateExchange(*Cast<DSkeleton>(Candidate.GetAsset()), Error);
					return Exchange ? std::make_unique<TOwnedSceneExchange<FSkeletonImportedStateExchange>>(std::move(Exchange)) : nullptr;
				}
				if (Kind == ESceneOutputKind::SkeletalMesh)
				{
					auto* Mesh = Cast<DSkeletalMesh>(Candidate.GetAsset());
					auto Exchange = Cast<DSkeletalMesh>(&Target)->PrepareImportedStateExchange(*Mesh, *Mesh->GetSkeleton(), Error);
					return Exchange ? std::make_unique<TOwnedSceneExchange<FSkeletalMeshImportedStateExchange>>(std::move(Exchange)) : nullptr;
				}
				auto* Clip = Cast<DAnimationClip>(Candidate.GetAsset());
				auto Exchange = Cast<DAnimationClip>(&Target)->PrepareImportedStateExchange(*Clip, *Clip->GetSkeleton(), Error);
				return Exchange ? std::make_unique<TOwnedSceneExchange<FAnimationClipImportedStateExchange>>(std::move(Exchange)) : nullptr;
			}

		private:
			auto Fail(const FImportFactoryNode& Node, std::string Message,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct>
			{
				Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Durin.Scene.ProductBuildFailed", .Phase = "ProductBuild",
					.OutputIdentity = Node.StableIdentity, .Message = std::move(Message)});
				return {};
			}
			auto MaterializationFailure(std::unique_ptr<FEngineSingleAssetCandidate> Candidate,
				std::string Message, std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate>
			{
				Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Durin.Scene.MaterializationFailed", .Phase = "Materialization",
					.Message = std::move(Message)});
				Candidate->Abandon();
				return {};
			}

			ESceneOutputKind Kind;
			mutable std::mutex PendingMutex;
			mutable std::unordered_map<DObject*, std::unique_ptr<FSceneInterchangeFactoryProduct>> Pending;
		};

		std::mutex GRegistrationMutex;
		FInterchangeRegistration GImageTranslatorRegistration;
		FInterchangeRegistration GTexture2DPipelineRegistration;
		FInterchangeRegistration GTexture2DFactoryRegistration;
		FInterchangeRegistration GGeometryTranslatorRegistration;
		FInterchangeRegistration GStaticMeshPipelineRegistration;
		FInterchangeRegistration GStaticMeshFactoryRegistration;
		FInterchangeRegistration GSceneTranslatorRegistration;
		FInterchangeRegistration GScenePipelineRegistration;
		std::vector<FInterchangeRegistration> GSceneFactoryRegistrations;
		std::vector<FInterchangeRegistration> GImageFamilyInterchangeRegistrations;
		uint32 GRegistrationReferenceCount = 0;
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
			std::optional<FInterchangeProvenance> Existing;
			if (Mesh.GetSourceImportData().HasSource())
			{
				FInterchangeProvenance Provenance;
				if (!InspectStaticMeshInterchangeProvenance(Mesh, Provenance, OutError))
					return false;
				Existing = std::move(Provenance);
			}
			FAssetPath Destination;
			if (!FAssetPath::TryCreate(
				Mesh.GetPackage()->GetPackagePath(), Destination, &OutError)) return false;
			FInterchangeImportRequest Request;
			if (!MakeStaticMeshInterchangeRequest(
				Source.SourcePath, Destination, Mesh.GetImportSettings(),
				EInterchangeImportMode::ReplaceSource,
				{.OwnerId = std::format("StaticMesh.ReplaceSource:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Existing), Request, OutError)) return false;
			const FInterchangeImportResult Result = GetImportService().RunInterchangeImportInline(
				std::move(Request),
				std::format("Replace StaticMesh source {}", Destination.GetAssetName()));
			if (Result.Outcome.State != EImportOperationState::Succeeded)
			{
				OutError = Result.Outcome.Diagnostic.empty()
					? "StaticMesh Interchange source replacement failed."
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
			FInterchangeImportRequest Request;
			if (!MakeStaticMeshInterchangeRequest(
				MountedSource.SourcePath, ParsedAssetPath, ImportSettings,
				EInterchangeImportMode::Import,
				{.OwnerId = std::format("StaticMesh.Import:{}", ParsedAssetPath.ToString()),
					.ConflictIdentities = {ParsedAssetPath.ToString()}},
				{}, Request, Error))
				return {false, std::move(Error), nullptr};
			const FInterchangeImportResult Imported = GetImportService().RunInterchangeImportInline(
				std::move(Request),
				std::format("Import StaticMesh {}", ParsedAssetPath.GetAssetName()));
			if (Imported.Outcome.State != EImportOperationState::Succeeded)
				return {false, Imported.Outcome.Diagnostic.empty()
					? "StaticMesh Interchange import failed."
					: Imported.Outcome.Diagnostic, nullptr};
			DObject* ImportedObject = nullptr;
			(void)Asset::LoadAsset(ParsedAssetPath, ImportedObject);
			auto* Mesh = Cast<DStaticMesh>(ImportedObject);
			if (!Mesh)
				return {false, "StaticMesh Interchange import published no destination asset.", nullptr};
			MountedSource.Commit();
			return {true, {}, Mesh};
		}
	auto FAssetForgeAuthoringFeatures::BuildFileProduct(
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

	auto FAssetForgeAuthoringFeatures::PostLoadUncooked(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		return PostLoadStaticMesh(Mesh, OutDiagnostic, OutError);
	}

	auto FAssetForgeAuthoringFeatures::ChangeSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return ChangeStaticMeshSourceReference(Mesh, SourceVirtualPath, OutError);
	}

	auto FAssetForgeAuthoringFeatures::PostLoadUncooked(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return PostLoadTexture2DFeature(Texture, OutError);
	}

	auto FAssetForgeAuthoringFeatures::WaitForRecovery(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		return WaitForTexture2DInterchangeRecovery(Texture, TimeoutSeconds);
	}

	auto FAssetForgeAuthoringFeatures::RecoverUncooked(
		DVolumeTexture& Texture, std::string& OutError) -> bool
	{
		const std::string Key = Asset::Build::MakeVolumeTextureDerivedDataKey(Texture, OutError);
		if (Key.empty()) return false;
		std::unique_ptr<FVolumeTexturePlatformData> Cached;
		ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
		std::string Message;
		if (Asset::Build::LoadVolumeTextureDerivedData(Key, Cached, Status, Message))
			return Texture.PublishDerivedDataLoad(std::move(Cached), Key, OutError);
		const FVolumeTextureSourceImportData& Source = Texture.GetSourceImportData();
		FAssetPath Destination;
		if (!Texture.GetPackage() || !Source.HasSource()
			|| !FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
			return false;
		const FVolumeTextureImportSettings Settings{
			.ImportFormat = Source.ImportFormat, .Channels = Source.Channels,
			.SliceWidth = Source.SliceWidth, .SliceHeight = Source.SliceHeight,
			.Depth = Source.Depth, .TilesX = Source.TilesX, .TilesY = Source.TilesY};
		FInterchangeProvenance Existing;
		std::optional<FInterchangeProvenance> Provenance;
		if (InspectVolumeTextureInterchangeProvenance(Texture, Existing, OutError))
			Provenance = std::move(Existing);
		else OutError.clear();
		FInterchangeImportRequest Request;
		if (!MakeVolumeTextureInterchangeRequest(Source.Source.SourcePath, Destination,
			Settings, EInterchangeImportMode::Recover,
			{.OwnerId = std::format("VolumeTexture.Recovery:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}},
			std::move(Provenance), Request, OutError)) return false;
		Request.Lifetime = EImportOperationLifetime::SessionCritical;
		const FInterchangeImportHandle Handle = GetImportService().SubmitInterchangeImport(
			std::move(Request), std::format("Recover VolumeTexture {}", Destination.GetAssetName()));
		if (!Handle)
		{
			OutError = "VolumeTexture Interchange recovery could not be submitted.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto MakeStaticMeshInterchangeRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FStaticMeshImportSettings& Settings,
		EInterchangeImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		if (MountedSource.IsEmpty() || !Destination.IsValid()
			|| !Settings.IsValid(&OutError))
		{
			if (OutError.empty()) OutError = "StaticMesh Interchange request is invalid.";
			return false;
		}
		const EImportOutputPolicy Policy = Mode == EInterchangeImportMode::Import
			|| Mode == EInterchangeImportMode::Preview
			? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "StaticMesh.Interchange";
		if (Owner.ConflictIdentities.empty())
			Owner.ConflictIdentities.push_back(Destination.ToString());
		std::optional<FInterchangeProvenance> PersistedProvenance = std::move(ExistingProvenance);
		std::string ProvenanceError;
		if (PersistedProvenance && !PersistedProvenance->Validate(ProvenanceError))
			PersistedProvenance.reset();
		OutRequest = {
			.Mode = Mode,
			.RootSource = MountedSource,
			.TranslatorId = std::string(GeometryTranslatorId),
			.TranslatorSettings = EncodeGeometryTranslatorSettings(Settings),
			.PipelineStack = {{
				.PipelineId = std::string(StaticMeshPipelineId),
				.ContractVersion = 1,
				.Settings = EncodeStaticMeshInterchangePlan({
					.Destination = Destination, .Settings = Settings, .Policy = Policy})}},
			.Destination = Destination,
			.Owner = std::move(Owner),
			.ExistingProvenance = std::move(PersistedProvenance)};
		OutError.clear();
		return true;
	}

	auto SubmitStaticMeshInterchangeImport(std::string_view FilePath,
		const FAssetPath& Destination, const FStaticMeshImportSettings& Settings,
		std::string_view SourceDestination, bool bEngineAuthoringContext,
		FInterchangeImportCompletion Completion, std::string& OutError)
		-> FInterchangeImportHandle
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
		FInterchangeImportRequest Request;
		if (!MakeStaticMeshInterchangeRequest(Mounted->SourcePath, Destination, Settings,
			EInterchangeImportMode::Import,
			{.OwnerId = std::format("StaticMesh.Import:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}}, {}, Request, OutError)) return {};
		OutError.clear();
		return GetImportService().SubmitInterchangeImport(std::move(Request),
			std::format("Import StaticMesh {}", Destination.GetAssetName()),
			[Mounted, Completion = std::move(Completion)](const FInterchangeImportResult& Result) {
				if (Result.Outcome.State == EImportOperationState::Succeeded) Mounted->Commit();
				if (Completion) Completion(Result);
			});
	}

	auto MakeSceneInterchangeRequest(
		const FSourcePath& MountedRootSource,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		EInterchangeImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		if (MountedRootSource.IsEmpty() || !DestinationDirectory.IsValid()
			|| !Settings.IsValid(&OutError))
		{
			if (OutError.empty()) OutError = "Scene Interchange request is invalid.";
			return false;
		}
		const EImportOutputPolicy Policy = Mode == EInterchangeImportMode::Import
			|| Mode == EInterchangeImportMode::Preview
			? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "Scene.Interchange";
		if (Owner.ConflictIdentities.empty())
			Owner.ConflictIdentities.push_back(DestinationDirectory.ToString());
		std::vector<FInterchangeOutputMapping> ExistingMappings;
		if (ExistingProvenance) ExistingMappings = ExistingProvenance->OutputMappings;
		const FInterchangePayload Plan = EncodeSceneInterchangePlan({
			.DestinationDirectory = DestinationDirectory,
			.MeshSettings = Settings, .DefaultPolicy = Policy,
			.ExistingMappings = std::move(ExistingMappings)});
		std::optional<FInterchangeProvenance> PersistedProvenance = std::move(ExistingProvenance);
		std::string ProvenanceError;
		if (PersistedProvenance && !PersistedProvenance->Validate(ProvenanceError))
			PersistedProvenance.reset();
		OutRequest = {
			.Mode = Mode, .RootSource = MountedRootSource,
			.TranslatorId = std::string(SceneTranslatorId), .TranslatorSettings = Plan,
			.PipelineStack = {{.PipelineId = std::string(ScenePipelineId),
				.ContractVersion = 1, .Settings = Plan}},
			.Destination = DestinationDirectory, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(PersistedProvenance)};
		OutError.clear();
		return true;
	}

	auto MakeSceneRecordInterchangeRequest(
		const DImportRecord& Record,
		EImportRecordAction,
		FImportOperationOwner Owner,
		FInterchangeImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		const auto Root = std::ranges::find(
			Record.GetSources(), std::string_view("root"),
			&FImportRecordSource::StableIdentity);
		if (Root == Record.GetSources().end())
		{
			OutError = "Scene import record has no root source.";
			return false;
		}

		FAssetPath DestinationDirectory;
		FStaticMeshImportSettings MeshSettings;
		if (Record.GetProviderId() == SceneTranslatorId)
		{
			FSceneInterchangePlan PersistedPlan;
			const FInterchangePayload Settings = MakeInterchangePayload(
				Record.GetSettings().SchemaId, Record.GetSettings().SchemaVersion,
				std::vector<std::byte>(Record.GetSettings().Bytes));
			if (!DecodeSceneInterchangePlan(Settings, PersistedPlan, OutError)) return false;
			DestinationDirectory = PersistedPlan.DestinationDirectory;
			MeshSettings = PersistedPlan.MeshSettings;
		}
		else if (Record.GetProviderId() == SceneImportProviderId
			&& Record.GetProviderContractVersion() == SceneImportProviderContractVersion
			&& Record.GetSettings().SchemaId == "Durin.Scene.ImportSettings"
			&& Record.GetSettings().SchemaVersion == 2)
		{
			std::span<const std::byte> Bytes(Record.GetSettings().Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, MeshSettings.ForwardAxis)
				|| !ReadValue(Bytes, MeshSettings.RightAxis)
				|| !ReadValue(Bytes, MeshSettings.UpAxis)
				|| !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, DestinationDirectory, &OutError)
				|| !MeshSettings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "Legacy Scene import settings are malformed.";
				return false;
			}
		}
		else
		{
			OutError = std::format("Scene import record provider '{}'/{} is unsupported.",
				Record.GetProviderId(), Record.GetProviderContractVersion());
			return false;
		}

		FInterchangeProvenance MappingCarrier;
		for (const FImportRecordOutput& Output : Record.GetOutputs())
			MappingCarrier.OutputMappings.push_back({
				.TranslatedNodeIdentity = Output.StableIdentity,
				.OutputIdentity = Output.StableIdentity,
				.AssetPath = Output.AssetPath});
		const std::string RecordPath = Record.GetPackage()
			? Record.GetPackage()->GetPackagePath() : std::string{};
		FAssetPath ParsedRecordPath;
		if (!FAssetPath::TryCreate(RecordPath, ParsedRecordPath, &OutError)) return false;
		MappingCarrier.OutputMappings.push_back({
			.TranslatedNodeIdentity = "scene-import-record",
			.OutputIdentity = "scene-import-record", .AssetPath = ParsedRecordPath});
		if (Owner.OwnerId.empty())
			Owner.OwnerId = std::format("Scene.RecordReimport:{}", RecordPath);
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(RecordPath);
		return MakeSceneInterchangeRequest(Root->SourcePath, DestinationDirectory,
			MeshSettings, EInterchangeImportMode::Reimport, std::move(Owner),
			std::move(MappingCarrier), OutRequest, OutError);
	}

	auto InspectStaticMeshInterchangeProvenance(
		const DStaticMesh& Mesh,
		FInterchangeProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		if (!Mesh.GetInterchangeProvenance().empty())
		{
			const std::string_view Hex = Mesh.GetInterchangeProvenance();
			if ((Hex.size() & 1) != 0)
			{
				OutError = "StaticMesh Interchange provenance encoding is malformed.";
				return false;
			}
			std::vector<std::byte> Bytes(Hex.size() / 2);
			auto Nibble = [](char Value) -> int {
				if (Value >= '0' && Value <= '9') return Value - '0';
				if (Value >= 'a' && Value <= 'f') return Value - 'a' + 10;
				return -1;
			};
			for (size_t Index = 0; Index < Bytes.size(); ++Index)
			{
				const int High = Nibble(Hex[Index * 2]);
				const int Low = Nibble(Hex[Index * 2 + 1]);
				if (High < 0 || Low < 0)
				{
					OutError = "StaticMesh Interchange provenance encoding is malformed.";
					return false;
				}
				Bytes[Index] = static_cast<std::byte>((High << 4) | Low);
			}
			return DeserializeInterchangeProvenance(Bytes, OutProvenance, OutError);
		}
		const FStaticMeshSourceImportData& Source = Mesh.GetSourceImportData();
		const bool bHashValid = Source.SourceContentHash.size() == 32
			&& std::ranges::all_of(Source.SourceContentHash, [](char Value) {
				return Value >= '0' && Value <= '9' || Value >= 'a' && Value <= 'f';
			});
		if (!Source.HasSource() || !bHashValid)
		{
			OutError = "StaticMesh has no compatible Interchange or legacy provenance.";
			return false;
		}
		const FXxHash128 Hash = FXxHash128::FromString(Source.SourceContentHash);
		FAssetPath AssetPath;
		if (!Mesh.GetPackage()
			|| !FAssetPath::TryCreate(Mesh.GetPackage()->GetPackagePath(), AssetPath, &OutError))
			return false;
		OutProvenance = {
			.Translator = {.Id = std::string(GeometryTranslatorId), .ContractVersion = 1,
				.Settings = EncodeGeometryTranslatorSettings(Source.ImportSettings)},
			.PipelineStack = {{.PipelineId = std::string(StaticMeshPipelineId),
				.ContractVersion = 1,
				.Settings = EncodeStaticMeshInterchangePlan({
					.Destination = AssetPath, .Settings = Source.ImportSettings,
					.Policy = EImportOutputPolicy::ReplaceWholeState})}},
			.Sources = {{.StableIdentity = "root", .Role = "Root",
				.SourcePath = Source.SourcePath, .ContentHash = Hash}},
			.OutputMappings = {{.TranslatedNodeIdentity = "mesh:combined",
				.OutputIdentity = "static-mesh", .AssetPath = AssetPath}},
			.AuthoredOutputFingerprint = Source.SourceContentHash};
		OutError.clear();
		return true;
	}

	auto MakeTexture2DInterchangeRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FTexture2DImportSettings& Settings,
		EInterchangeImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest,
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
			OutError = "Texture2D Interchange request settings are invalid.";
			return false;
		}
		const EImportOutputPolicy Policy = Mode == EInterchangeImportMode::Import
			|| Mode == EInterchangeImportMode::Preview
			? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "Texture2D.Interchange";
		if (Owner.ConflictIdentities.empty())
			Owner.ConflictIdentities.push_back(Destination.ToString());
		OutRequest = {
			.Mode = Mode,
			.RootSource = MountedSource,
			.TranslatorId = std::string(ImageTranslatorId),
			.TranslatorSettings = MakeInterchangePayload(
				std::string(EmptyTranslatorSettingsSchema), 1, {}),
			.PipelineStack = {{
				.PipelineId = std::string(Texture2DPipelineId),
				.ContractVersion = 1,
				.Settings = EncodeTexture2DInterchangePlan({
					.Destination = Destination,
					.Settings = Settings,
					.Policy = Policy})}},
			.Destination = Destination,
			.Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		OutError.clear();
		return true;
	}

	auto InspectTexture2DInterchangeProvenance(
		const DTexture2D& Texture,
		FInterchangeProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		if (!Texture.GetInterchangeProvenance().empty())
		{
			const std::string_view Hex = Texture.GetInterchangeProvenance();
			if ((Hex.size() & 1) != 0)
			{
				OutError = "Texture2D Interchange provenance encoding is malformed.";
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
					OutError = "Texture2D Interchange provenance encoding is malformed.";
					return false;
				}
				Bytes[Index] = static_cast<std::byte>((High << 4) | Low);
			}
			return DeserializeInterchangeProvenance(Bytes, OutProvenance, OutError);
		}
		const FTexture2DSourceImportData& Source = Texture.GetSourceImportData();
		FAssetPath Destination;
		if (!Texture.GetPackage() || !Source.HasSource()
			|| (Source.DecoderId != "DurinImage" && Source.DecoderId != ImageTranslatorId)
			|| !FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination,
				&OutError))
		{
			if (OutError.empty())
				OutError = "Texture2D has no compatible Interchange or legacy image provenance.";
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
				.Settings = MakeInterchangePayload(
					std::string(EmptyTranslatorSettingsSchema), 1, {})},
			.PipelineStack = {{
				.PipelineId = std::string(Texture2DPipelineId), .ContractVersion = 1,
				.Settings = EncodeTexture2DInterchangePlan({
					.Destination = Destination, .Settings = Settings,
					.Policy = EImportOutputPolicy::ReplaceWholeState})}},
			.Sources = {{
				.StableIdentity = "root", .Role = "Root",
				.SourcePath = Source.Source.SourcePath,
				.ContentHash = {.HashLow = Source.Source.SourceContentHashLow,
					.HashHigh = Source.Source.SourceContentHashHigh},
				.ByteCount = Texture.GetSourceFileSize()}},
			.OutputMappings = {{
				.TranslatedNodeIdentity = "image", .OutputIdentity = "texture2d",
				.AssetPath = Destination}},
			.AuthoredOutputFingerprint = Texture.GetDerivedDataKey()};
		OutError.clear();
		return true;
	}

	auto FAssetForgeAuthoringFeatures::PostLoadUncooked(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeFeature(Texture, OutError);
	}

	auto RegisterAssetForgeProviders(
		std::string& OutError, FModuleOwnedCallbackGate OwnerGate) -> bool
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (GImageTranslatorRegistration && GTexture2DPipelineRegistration
			&& GTexture2DFactoryRegistration && GGeometryTranslatorRegistration
			&& GStaticMeshPipelineRegistration && GStaticMeshFactoryRegistration
			&& GSceneTranslatorRegistration && GScenePipelineRegistration
			&& GSceneFactoryRegistrations.size() == SceneFactoryIds.size()
			&& GImageFamilyInterchangeRegistrations.size() == 8)
		{
			++GRegistrationReferenceCount;
			OutError.clear();
			return true;
		}
		auto& Service = GetImportService();
			auto RollbackFrameworkRegistrations = [&] {
			GImageFamilyInterchangeRegistrations.clear();
			GSceneFactoryRegistrations.clear();
			GScenePipelineRegistration.Reset();
			GSceneTranslatorRegistration.Reset();
			GStaticMeshFactoryRegistration.Reset();
			GStaticMeshPipelineRegistration.Reset();
			GGeometryTranslatorRegistration.Reset();
			GTexture2DFactoryRegistration.Reset();
			GTexture2DPipelineRegistration.Reset();
			GImageTranslatorRegistration.Reset();
		};
		GImageTranslatorRegistration = Service.RegisterTranslatorScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(ImageTranslatorId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(EmptyTranslatorSettingsSchema),
						.SchemaVersion = 1}},
				.Extensions = {".png", ".jpg", ".jpeg", ".bmp", ".tga"},
				.Priority = 100,
				.TranslationThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FImageInterchangeTranslator>()},
			OwnerGate, OutError);
		if (!GImageTranslatorRegistration)
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GTexture2DPipelineRegistration = Service.RegisterPipelineScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(Texture2DPipelineId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(Texture2DPlanSchema),
						.SchemaVersion = 1}},
				.Priority = 100,
				.ExecutionThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FDefaultTexture2DInterchangePipeline>()},
			OwnerGate, OutError);
		if (!GTexture2DPipelineRegistration)
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GTexture2DFactoryRegistration = Service.RegisterFactoryScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(Texture2DFactoryId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(Texture2DPlanSchema),
						.SchemaVersion = 1}},
				.OutputClassName = "Durin::DTexture2D",
				.Priority = 100,
				.ProductBuildThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTexture2DInterchangeFactory>()},
			OwnerGate, OutError);
		if (!GTexture2DFactoryRegistration)
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GGeometryTranslatorRegistration = Service.RegisterTranslatorScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(GeometryTranslatorId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(GeometryTranslatorSettingsSchema),
						.SchemaVersion = 1}},
				.Extensions = {".obj", ".fbx", ".gltf", ".glb",
					".dae", ".3ds", ".ply", ".stl"},
				.Priority = 100,
				.TranslationThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FGeometryInterchangeTranslator>()},
			OwnerGate, OutError);
		if (!GGeometryTranslatorRegistration)
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GStaticMeshPipelineRegistration = Service.RegisterPipelineScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(StaticMeshPipelineId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(StaticMeshPlanSchema),
						.SchemaVersion = 1}},
				.Priority = 100,
				.ExecutionThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FDefaultStaticMeshInterchangePipeline>()},
			OwnerGate, OutError);
		if (!GStaticMeshPipelineRegistration)
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GStaticMeshFactoryRegistration = Service.RegisterFactoryScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(StaticMeshFactoryId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(StaticMeshPlanSchema),
						.SchemaVersion = 1}},
				.OutputClassName = "Durin::DStaticMesh",
				.Priority = 100,
				.ProductBuildThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FStaticMeshInterchangeFactory>()},
			OwnerGate, OutError);
		if (!GStaticMeshFactoryRegistration)
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GSceneTranslatorRegistration = Service.RegisterTranslatorScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(SceneTranslatorId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(ScenePlanSchema), .SchemaVersion = 1}},
				.Extensions = {".gltf", ".glb", ".fbx", ".obj", ".dae", ".3ds", ".ply", ".stl"},
				.Priority = 110,
				.TranslationThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FSceneInterchangeTranslator>()}, OwnerGate, OutError);
		if (!GSceneTranslatorRegistration) { RollbackFrameworkRegistrations(); return false; }
		GScenePipelineRegistration = Service.RegisterPipelineScoped({
			.Descriptor = {
				.Identity = {.Id = std::string(ScenePipelineId), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(ScenePlanSchema), .SchemaVersion = 1}},
				.Priority = 110,
				.ExecutionThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FDefaultSceneInterchangePipeline>()}, OwnerGate, OutError);
		if (!GScenePipelineRegistration) { RollbackFrameworkRegistrations(); return false; }
		GSceneFactoryRegistrations.reserve(SceneFactoryIds.size());
		for (size_t Index = 0; Index < SceneFactoryIds.size(); ++Index)
		{
			const ESceneOutputKind Kind = Index == 0 ? ESceneOutputKind::StaticMesh
				: Index == 1 ? ESceneOutputKind::MaterialInstance
				: Index == 2 ? ESceneOutputKind::Skeleton
				: Index == 3 ? ESceneOutputKind::SkeletalMesh
				: Index == 4 ? ESceneOutputKind::AnimationClip
				: Index == 5 ? ESceneOutputKind::Texture2D : ESceneOutputKind::ImportRecord;
			auto Registration = Service.RegisterFactoryScoped({
				.Descriptor = {
					.Identity = {.Id = std::string(SceneFactoryIds[Index]), .ContractVersion = 1,
						.Settings = {.SchemaId = std::string(ScenePlanSchema), .SchemaVersion = 1}},
					.OutputClassName = std::string(SceneOutputClassName(Kind)), .Priority = 110,
					.ProductBuildThread = EInterchangeThreadCapability::WorkerSafe},
				.Implementation = std::make_shared<FSceneInterchangeFactory>(Kind)}, OwnerGate, OutError);
			if (!Registration) { RollbackFrameworkRegistrations(); return false; }
			GSceneFactoryRegistrations.push_back(std::move(Registration));
		}
		if (!RegisterImageFamilyInterchange(Service, OwnerGate,
			GImageFamilyInterchangeRegistrations, OutError))
		{
			RollbackFrameworkRegistrations();
			return false;
		}
		GRegistrationReferenceCount = 1;
		OutError.clear();
		return true;
	}

	auto UnregisterAssetForgeProviders() -> void
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (GRegistrationReferenceCount > 1)
		{
			--GRegistrationReferenceCount;
			return;
		}
		GRegistrationReferenceCount = 0;
		if (!GImageTranslatorRegistration && !GTexture2DPipelineRegistration
			&& !GTexture2DFactoryRegistration && !GGeometryTranslatorRegistration
			&& !GStaticMeshPipelineRegistration && !GStaticMeshFactoryRegistration
			&& GImageFamilyInterchangeRegistrations.empty()) return;
		GImageFamilyInterchangeRegistrations.clear();
		GSceneFactoryRegistrations.clear();
		GScenePipelineRegistration.Reset();
		GSceneTranslatorRegistration.Reset();
		{
			std::lock_guard CacheLock(GSceneInterchangeCacheMutex);
			GSceneInterchangeCache.clear();
			GSceneInterchangeOutputCache.clear();
		}
		GStaticMeshFactoryRegistration.Reset();
		GStaticMeshPipelineRegistration.Reset();
		GGeometryTranslatorRegistration.Reset();
		GTexture2DFactoryRegistration.Reset();
		GTexture2DPipelineRegistration.Reset();
		GImageTranslatorRegistration.Reset();
	}
}
