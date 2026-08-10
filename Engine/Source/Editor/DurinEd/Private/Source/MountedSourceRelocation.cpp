#include "Source/MountedSourceRelocation.h"

#include "AssetSystem.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		struct FPackageSnapshot
		{
			Asset::FAssetData Data;
			std::vector<uint8> Bytes;
			DObject* Asset = nullptr;
			bool bWasLoaded = false;
			bool bUpdated = false;
		};

		auto SaveBytesAtomically(
			const std::filesystem::path& Path,
			std::span<const uint8> Bytes,
			std::string& OutError) -> bool
		{
			FFileHelper::FAtomicFileError Error;
			const std::span<const std::byte> ByteView(
				reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size());
			if (FFileHelper::SaveArrayToFileAtomically(ByteView, Path, &Error))
				return true;
			OutError = Error.ToString();
			return false;
		}

		auto ChangeAssetSourceReference(
			DObject* Asset,
			std::string_view From,
			std::string_view To,
			std::string& OutError) -> bool
		{
			if (DTexture2D* Texture = Cast<DTexture2D>(Asset))
			{
				if (!Texture->ChangeSourceReference(To, OutError)) return false;
				if (Texture->WaitForPendingBuild()) return true;
				OutError = Texture->GetLastBuildError().empty()
					? "Texture2D source relocation build did not complete."
					: Texture->GetLastBuildError();
				return false;
			}
			if (DStaticMesh* Mesh = Cast<DStaticMesh>(Asset))
				return Mesh->ChangeSourceReference(To, OutError);
			if (DTextureCube* Cube = Cast<DTextureCube>(Asset))
			{
				if (Cube->GetSourceLayout()
					== ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					if (Cube->GetPanoramaSourceFile() != From)
					{
						OutError = "TextureCube panorama no longer references the source being relocated.";
						return false;
					}
					return Cube->ChangePanoramaSourceReference(
						To,
						{
							.FaceDimension = Cube->GetPanoramaFaceDimension(),
							.ExposureEV = Cube->GetPanoramaExposureEV()},
						OutError);
				}
				std::array<std::string, TextureCubeFaceCount> Paths;
				bool bFound = false;
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					Paths[Index] =
						Cube->GetSourceFile(static_cast<ETextureCubeFace>(Index));
					if (Paths[Index] == From)
					{
						Paths[Index] = To;
						bFound = true;
					}
				}
				if (!bFound)
				{
					OutError = "TextureCube faces no longer reference the source being relocated.";
					return false;
				}
				return Cube->ChangeSourceReferences(
					Paths, {.bSRGB = Cube->IsSRGB()}, OutError);
			}
			OutError = "The source relocation contains an unsupported asset class.";
			return false;
		}

		auto RollbackPackages(
			std::vector<FPackageSnapshot>& Snapshots,
			std::string_view Destination,
			std::string_view Original) -> void
		{
			for (FPackageSnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.bUpdated && Snapshot.Asset)
				{
					std::string RestoreError;
					ChangeAssetSourceReference(
						Snapshot.Asset, Destination, Original, RestoreError);
					if (Snapshot.Asset->GetPackage())
						Snapshot.Asset->GetPackage()->ClearDirty();
				}
				if (Snapshot.bUpdated)
				{
					std::string RestoreError;
					SaveBytesAtomically(
						Snapshot.Data.PhysicalPath,
						Snapshot.Bytes, RestoreError);
				}
				if (!Snapshot.bWasLoaded)
					Asset::UnloadPackage(Snapshot.Data.PackagePath);
			}
			Asset::GetAssetRegistry().ScanMountedContent(
				Asset::EAssetRegistryScanMode::FullValidation);
		}
	} // namespace

	auto RelocateMountedSourceAcrossPackages(
		const FMountedSourceRelocationRequest& Request,
		std::string& OutError) -> bool
	{
		if (Request.AffectedAssets.empty())
		{
			OutError = "No referencing packages were supplied for source relocation.";
			return false;
		}
		if (Request.AffectedAssets.size() > Request.MaximumAffectedPackages)
		{
			OutError = std::format(
				"Source relocation affects {} packages, exceeding the transaction limit of {}.",
				Request.AffectedAssets.size(), Request.MaximumAffectedPackages);
			return false;
		}

		std::vector<FPackageSnapshot> Snapshots;
		Snapshots.reserve(Request.AffectedAssets.size());
		for (const FSourceReference& Reference : Request.AffectedAssets)
		{
			const PathUtilities::FMountPolicyResult Dependency =
				PathUtilities::CheckMountDependency(
					Reference.AssetPath.GetView(),
					Request.DestinationSourceVirtualPath);
			if (!Dependency)
			{
				OutError = std::format(
					"{} cannot reference the relocation destination: {}",
					Reference.AssetPath.ToString(), Dependency.Message);
				return false;
			}
			const Asset::FAssetData* Data =
				Asset::GetAssetRegistry().FindAssetExact(Reference.AssetPath);
			if (!Data)
			{
				OutError = std::format(
					"Affected asset is no longer registered: {}",
					Reference.AssetPath.ToString());
				return false;
			}
			DPackage* Loaded = Asset::FindLoadedPackage(Reference.AssetPath);
			if (Loaded && Loaded->IsDirty())
			{
				OutError = std::format(
					"Save or discard changes to {} before relocating its shared source.",
					Reference.AssetPath.ToString());
				return false;
			}
			FPackageSnapshot& Snapshot = Snapshots.emplace_back();
			Snapshot.Data = *Data;
			Snapshot.bWasLoaded = Loaded != nullptr;
			if (!FFileHelper::LoadFileToArray(
				Snapshot.Bytes, Snapshot.Data.PhysicalPath))
			{
				OutError = std::format(
					"Failed to snapshot affected package {}.",
					Reference.AssetPath.ToString());
				return false;
			}
		}

		FMountedSourceRelocation Relocation;
		if (!PrepareMountedSourceRelocation(
			Request.AuthoringAssetPath,
			Request.OriginalSourceVirtualPath,
			Request.DestinationSourceVirtualPath,
			Relocation, OutError))
			return false;

		for (FPackageSnapshot& Snapshot : Snapshots)
		{
			Asset::FAssetResult LoadResult =
				Asset::LoadAsset(Snapshot.Data.PackagePath, Snapshot.Asset);
			if (!LoadResult)
			{
				OutError = LoadResult.Message;
				RollbackMountedSourceRelocation(Relocation);
				RollbackPackages(
					Snapshots, Request.DestinationSourceVirtualPath,
					Request.OriginalSourceVirtualPath);
				return false;
			}
			if (!ChangeAssetSourceReference(
				Snapshot.Asset,
				Request.OriginalSourceVirtualPath,
				Request.DestinationSourceVirtualPath,
				OutError))
			{
				RollbackMountedSourceRelocation(Relocation);
				RollbackPackages(
					Snapshots, Request.DestinationSourceVirtualPath,
					Request.OriginalSourceVirtualPath);
				return false;
			}
			Snapshot.bUpdated = true;
			const Asset::FAssetResult SaveResult =
				Asset::SavePackage(Snapshot.Asset->GetPackage());
			if (!SaveResult)
			{
				OutError = SaveResult.Message;
				RollbackMountedSourceRelocation(Relocation);
				RollbackPackages(
					Snapshots, Request.DestinationSourceVirtualPath,
					Request.OriginalSourceVirtualPath);
				return false;
			}
		}

		if (!CommitMountedSourceRelocation(Relocation, OutError))
		{
			RollbackMountedSourceRelocation(Relocation);
			RollbackPackages(
				Snapshots, Request.DestinationSourceVirtualPath,
				Request.OriginalSourceVirtualPath);
			return false;
		}
		for (FPackageSnapshot& Snapshot : Snapshots)
			if (!Snapshot.bWasLoaded)
				Asset::UnloadPackage(Snapshot.Data.PackagePath);
		OutError.clear();
		return true;
	}
} // namespace Durin
