#include "AssetRegistryStateInternal.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/ObjectKey.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "Misc/MountPaths.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto PackageFilename(const FPackagePath& Path) -> std::string
		{
			const auto Resolved = FMountPaths::ResolveAssetPath(Path.GetView());
			return Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{};
		}

		// Value metadata and stable, non-owning identities only. Residency stays in CoreDObject.
		struct FAssetQueryState
		{
			uint64 PackageRevision = 0, DiskRevision = 0, MountRevision = 0;
			FAssetQuerySnapshot Snapshot{.Revision = 1};
			std::unordered_map<FObjectKey, FAssetData> Live;
			std::unordered_set<FPackagePath> LivePackages;
			FContentChangeJournal Changes;

			auto Synchronize() -> void
			{
				if (GIsGameThreadIdInitialized) CheckGameThread();
				const auto Packages = GetAssetPackageRevision();
				const auto Disk = GetAssetCatalogRevision();
				const auto Mounts = FMountPaths::GetMountRegistryRevision();
				if (Packages == PackageRevision && Disk == DiskRevision && Mounts == MountRevision) return;

				std::unordered_map<FObjectKey, FAssetData> Current;
				std::unordered_set<FPackagePath> CurrentPackages;
				for (auto* Package : GetLiveAssetPackages())
				{
					if (Package->IsPlayInEditor()) continue;
					if (!FMountPaths::FindMountForVirtualPath(Package->GetPackagePath())) continue;
					CurrentPackages.insert(Package->GetPackagePathIdentity());
					for (auto* Asset : Package->GetTopLevelAssets())
					{
						if (Asset->IsGarbage() || Asset->IsTemplateObject() || Asset->HasAnyObjectFlags(EObjectFlags::Transient)) continue;
						FAssetData Data;
						require(FTopLevelAssetPath::TryCreate(Asset->GetObjectPath(), Data.AssetPath));
						Data.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString();
						Data.PrimaryAssetId = Asset->GetPrimaryAssetId();
						Data.AssetBundleData = Asset->GetAssetBundleData();
						if (Data.AssetClassName == "Durin::DAssetRedirector")
						{
							const auto* Property = static_cast<FObjectProperty*>(Asset->GetClass()->FindPropertyByName("DestinationObject"));
							if (auto* Target = Property->GetObjectPropertyValue(Asset))
								require(FSoftObjectPath::TryCreate(Target->GetObjectPath(), Data.RedirectDestination));
						}
						Current.emplace(FObjectKey(Asset), std::move(Data));
					}
				}

				if (Current == Live && CurrentPackages == LivePackages && Disk == DiskRevision && Mounts == MountRevision)
				{
					PackageRevision = Packages;
					return;
				}

				FContentChangeBatch Batch{Snapshot.Revision, Snapshot.Revision + 1};
				if (DiskRevision != Disk)
				{
					const auto DiskChanges = CaptureAssetCatalogChanges(DiskRevision);
					Batch.bFullRefresh = DiskChanges.bFullRefresh;
					Batch.Changes = DiskChanges.Changes;
				}
				if (MountRevision != Mounts) Batch.bFullRefresh = true;
				const auto DiskSnapshot = CaptureAssetCatalogSnapshot();
				FAssetQuerySnapshot Next{.Revision = Batch.ToRevision, .Packages = DiskSnapshot.Assets};
				for (const auto& [Path, Package] : Next.Packages)
					if (!CurrentPackages.contains(Path))
						for (const auto& Asset : Package.TopLevelAssets) Next.Assets.emplace(Asset.AssetPath, Asset);
				for (const auto& [Key, Asset] : Current) Next.Assets.insert_or_assign(Asset.AssetPath, Asset);

				const auto Append = [&](EContentChangeKind Kind, const FAssetData* Before, const FAssetData* After) {
					Batch.Changes.push_back({Kind,
						Before ? PackageFilename(Before->AssetPath.GetPackagePath()) : std::string{},
						After ? PackageFilename(After->AssetPath.GetPackagePath()) : std::string{},
						Before ? Before->AssetPath.ToString() : std::string{},
						After ? After->AssetPath.ToString() : std::string{}});
				};
				for (const auto& [Key, Before] : Live)
				{
					const auto Found = Current.find(Key);
					if (Found == Current.end())
					{
						if (const auto* After = Next.FindAssetExact(Before.AssetPath)) Append(EContentChangeKind::Modified, &Before, After);
						else Append(EContentChangeKind::Removed, &Before, nullptr);
					}
					else if (Before.AssetPath != Found->second.AssetPath) Append(EContentChangeKind::Renamed, &Before, &Found->second);
					else if (Before != Found->second) Append(EContentChangeKind::Modified, &Before, &Found->second);
				}
				for (const auto& [Key, After] : Current)
					if (!Live.contains(Key))
					{
						if (const auto* Before = Snapshot.FindAssetExact(After.AssetPath)) Append(EContentChangeKind::Modified, Before, &After);
						else Append(EContentChangeKind::Added, nullptr, &After);
					}
				// An empty live package can hide disk exports without adding a live object.
				if (Batch.Changes.empty() && Next.Assets != Snapshot.Assets)
				{
					for (const auto& [Path, Asset] : Snapshot.Assets)
						if (!Next.Assets.contains(Path)) Append(EContentChangeKind::Removed, &Asset, nullptr);
					for (const auto& [Path, Asset] : Next.Assets)
						if (!Snapshot.Assets.contains(Path)) Append(EContentChangeKind::Added, nullptr, &Asset);
				}
				PackageRevision = Packages; DiskRevision = Disk; MountRevision = Mounts;
				Live = std::move(Current);
				LivePackages = std::move(CurrentPackages);
				// Authored edits that do not alter metadata do not invalidate discovery caches.
				if (Batch.Changes.empty() && !Batch.bFullRefresh) return;
				Snapshot = std::move(Next);
				Changes.Append(std::move(Batch));
			}
		};

		auto QueryState() -> FAssetQueryState&
		{
			static FAssetQueryState State;
			State.Synchronize();
			return State;
		}
	}

	auto CaptureAssetQuerySnapshot(EAssetQuerySource Source) -> FAssetQuerySnapshot
	{
		if (Source == EAssetQuerySource::OnDisk)
		{
			auto Disk = CaptureAssetCatalogSnapshot();
			FAssetQuerySnapshot Result{.Revision = Disk.Revision, .Packages = std::move(Disk.Assets)};
			for (const auto& [Path, Package] : Result.Packages)
				for (const auto& Asset : Package.TopLevelAssets) Result.Assets.emplace(Asset.AssetPath, Asset);
			return Result;
		}
		auto& State = QueryState();
		auto Result = State.Snapshot;
		if (Source == EAssetQuerySource::InMemory)
		{
			Result.Assets.clear();
			for (const auto& [Key, Asset] : State.Live) Result.Assets.emplace(Asset.AssetPath, Asset);
		}
		return Result;
	}

	auto FindAssetExact(const FTopLevelAssetPath& Path, EAssetQuerySource Source) -> FAssetCatalogEntry
	{
		if (Source == EAssetQuerySource::OnDisk) return AssetPrivate::GetAssetRegistryState().FindAssetExact(Path);
		auto& State = QueryState();
		const auto* Asset = State.Snapshot.FindAssetExact(Path);
		if (Source == EAssetQuerySource::InMemory && std::ranges::none_of(State.Live, [&](const auto& Entry) { return Entry.second.AssetPath == Path; })) Asset = nullptr;
		FAssetCatalogEntry Result{.Revision = State.Snapshot.Revision};
		if (Asset) Result.Asset = *Asset;
		if (const auto* Package = State.Snapshot.FindPackageExact(Path.GetPackagePath())) Result.Package = *Package;
		return Result;
	}

	auto GetAssetQueryRevision() -> uint64 { return QueryState().Snapshot.Revision; }
	auto CaptureAssetQueryChanges(uint64 FromRevision) -> FContentChangeBatch
	{
		auto& State = QueryState();
		return State.Changes.Read(FromRevision, State.Snapshot.Revision);
	}
}
