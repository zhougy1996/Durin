#include "Source/MountedSourceRelocation.h"

#include "AssetAuthoring.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin::Editor
{
	using Asset::CommitMountedSourceRelocation;
	using Asset::FMountedSourceRelocation;
	using Asset::PrepareMountedSourceRelocation;
	using Asset::RollbackMountedSourceRelocation;
	namespace
	{
		std::mutex GSourceRelocationHandlersMutex;
		std::unordered_map<
			FMountedSourceRelocationHandlerHandle,
			FMountedSourceRelocationHandler> GSourceRelocationHandlers;
		FMountedSourceRelocationHandlerHandle GNextSourceRelocationHandlerHandle = 1;

		auto SnapshotSourceRelocationHandlers()
			-> std::vector<FMountedSourceRelocationHandler>
		{
			std::lock_guard Lock(GSourceRelocationHandlersMutex);
			std::vector<FMountedSourceRelocationHandler> Handlers;
			Handlers.reserve(GSourceRelocationHandlers.size());
			for (const auto& [Handle, Handler] : GSourceRelocationHandlers)
				Handlers.push_back(Handler);
			return Handlers;
		}

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
			for (const FMountedSourceRelocationHandler& Handler
				: SnapshotSourceRelocationHandlers())
			{
				if (std::optional<bool> Result = Handler(*Asset, From, To, OutError))
					return *Result;
			}
			if (DStaticMesh* Mesh = Cast<DStaticMesh>(Asset))
			{
				if (Mesh->GetSourceFile() != From)
				{
					OutError = "StaticMesh no longer references the source being relocated.";
					return false;
				}
				return InvokeStaticMeshSourceChangeHandler(*Mesh, To, OutError);
			}
			if (DTerrainHeightmap* Heightmap = Cast<DTerrainHeightmap>(Asset))
			{
				if (Heightmap->GetSourceFile() != From)
				{
					OutError = "Terrain heightmap no longer references the source being relocated.";
					return false;
				}
				return InvokeTerrainHeightmapSourceChangeHandler(*Heightmap, To, OutError);
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
			const Asset::FAssetCatalogRefreshResult Refresh =
				Asset::RefreshAssetCatalog(
					Asset::EAssetRegistryScanMode::FullValidation);
			if (!Refresh)
				DURIN_WARN(
					"Mounted-source rollback retained asset catalog revision {} with {} error(s).",
					Refresh.ResultingRevision, Refresh.Errors.size());
		}
	} // namespace

	auto RegisterMountedSourceRelocationHandler(
		FMountedSourceRelocationHandler Handler)
		-> FMountedSourceRelocationHandlerHandle
	{
		if (!Handler) return 0;
		std::lock_guard Lock(GSourceRelocationHandlersMutex);
		const FMountedSourceRelocationHandlerHandle Handle =
			GNextSourceRelocationHandlerHandle++;
		GSourceRelocationHandlers.emplace(Handle, std::move(Handler));
		return Handle;
	}

	auto UnregisterMountedSourceRelocationHandler(
		FMountedSourceRelocationHandlerHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::lock_guard Lock(GSourceRelocationHandlersMutex);
		GSourceRelocationHandlers.erase(Handle);
	}

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
			const Asset::FAssetCatalogEntry Entry =
				Asset::FindAssetExact(Reference.AssetPath);
			const Asset::FAssetData* Data = Entry.Data ? &*Entry.Data : nullptr;
			if (!Data)
			{
				OutError = std::format(
					"Affected asset is no longer registered: {}",
					Reference.AssetPath.ToString());
				return false;
			}
			DPackage* Loaded = Asset::FindResidentPackage(Reference.AssetPath);
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
} // namespace Durin::Editor
