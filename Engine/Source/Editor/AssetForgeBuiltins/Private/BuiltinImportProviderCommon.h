#pragma once

#include "AssetForgeBuiltinsProviders.h"
#include "AssetForge/ImportService.h"
#include "AssetForgeBuiltinsAuthoringFeatures.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture2DBuildAdapter.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/VolumeTextureImport.h"
#include "Texture2DPostLoad.h"
#include "TextureCubePostLoadPolicy.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"

#include "Animation/AnimationClip.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "Asset/MountedSource.h"
#include "AssetForge/ImportTypes.h"
#include "AssetAuthoring.h"
#include "DObject/ObjectLifecycle.h"
#include "EncodedSourceSnapshot.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "SceneImportInternal.h"
#include "AssetForge/Operations/ImportOperation.h"
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
#include "ImageFamilyImports.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
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

		auto MakeSchemaPayload(
			std::string SchemaId, uint32 Version, std::vector<std::byte> Bytes)
			-> FSchemaPayload
		{
			FSchemaPayload Payload{
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
			FImportProvenance Existing;
			std::optional<FImportProvenance> Provenance;
			if (InspectStaticMeshImportProvenance(Mesh, Existing, OutError))
				Provenance = std::move(Existing);
			else OutError.clear();
			FImportRequest Request;
			if (!MakeStaticMeshImportRequest(Source.SourcePath, Destination,
				Source.ImportSettings, EImportMode::Recover,
				{.OwnerId = std::format("StaticMesh.Recovery:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Provenance), Request, OutError)) return false;
			Request.Lifetime = EImportOperationLifetime::SessionCritical;
			const FImportHandle Handle = GetImportService().SubmitImport(
				std::move(Request), std::format("Recover StaticMesh {}", Destination.GetAssetName()));
			if (!Handle)
			{
				OutError = "StaticMesh AssetForge recovery could not be submitted.";
				return false;
			}
			OutDiagnostic = {
				.Status = EStaticMeshDerivedDataStatus::Missing,
				.Message = "Scheduled SessionCritical StaticMesh AssetForge recovery.",
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
					else if (const auto* Record = Cast<AssetForge::DImportRecord>(AssetObject))
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

		}
}
