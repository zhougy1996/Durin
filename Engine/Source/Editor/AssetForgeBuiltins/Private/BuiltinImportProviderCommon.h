#pragma once

#include "BuiltinImportSchema.h"
#include "DObject/Package.h"
#include "BuiltinSingleAssetImport.h"
#include "AssetForgeBuiltinsProviders.h"
#include "AssetForge/ImportService.h"
#include "AssetForgeBuiltinsAssetFeatures.h"
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
#include "StaticMesh/StaticMeshBuild.h"
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

		auto MakeStaticMeshSettings(const FStaticMeshImportSettings& Settings) -> FImportPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Settings.ForwardAxis);
			AppendValue(Bytes, Settings.RightAxis);
			AppendValue(Bytes, Settings.UpAxis);
			return MakeImportPayload("Durin.StaticMesh.ImportSettings", 1, std::move(Bytes));
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
			Asset::FStaticMeshImportedData& OutData,
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
			FStaticMeshBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			Asset::FStaticMeshImportedData ImportedData;
			if (!DecodeStaticMeshSource(
				FilePath, SourceImportData.ImportSettings, ImportedData, OutError))
				return false;
			return Asset::FStaticMeshBuildOperations::BuildImportedProduct(
				Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
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
			FStaticMeshBuildProduct Product;
			EStaticMeshDerivedDataStatus CacheStatus =
				EStaticMeshDerivedDataStatus::Missing;
			std::string CacheMessage;
			if (!bSourceMetadataStale
				&& Asset::FStaticMeshBuildOperations::LoadDerivedDataProduct(
					Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
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
			return MakeImportPayload("Durin.Texture2D.ImportSettings", 1, std::move(Bytes));
		}

		auto MakeTextureCubeSettings(const DTextureCube& Texture) -> FImportPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Texture.GetSourceLayout());
			AppendValue(Bytes, Texture.GetPanoramaFaceDimension());
			AppendValue(Bytes, Texture.GetPanoramaExposureEV());
			const bool bSRGB = Texture.IsSRGB();
			AppendValue(Bytes, bSRGB);
			return MakeImportPayload("Durin.TextureCube.ImportSettings", 1, std::move(Bytes));
		}

		auto MakeTerrainHeightmapSettings() -> FImportPayload
		{
			return MakeImportPayload("Durin.TerrainHeightmap.ImportSettings", 1, {});
		}

		auto MakeSourceHash(const FTextureSourceFile& Source) -> FXxHash128
		{
			FXxHash128 Hash;
			Hash.HashLow = Source.SourceContentHashLow;
			Hash.HashHigh = Source.SourceContentHashHigh;
			return Hash;
		}

		}
}
