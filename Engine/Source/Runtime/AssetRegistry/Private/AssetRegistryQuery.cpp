#include "AssetRegistryStateInternal.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/ObjectKey.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "Misc/MountPaths.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

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
			bool bMountsChanged = true;
			FContentChangeBatch PendingCatalogChanges;
			std::atomic_bool Dirty = true;
			std::mutex PublicationMutex;
			std::weak_ptr<void> PendingPublication;
			auto Invalidate() -> void
			{
				Dirty = true;
				std::lock_guard Lock(PublicationMutex);
				if (IsTaskSchedulerRunning() && GetGameThreadDeferredWorkQueueDiagnostics().bAccepting && PendingPublication.expired())
				{
					auto Ticket = std::make_shared<int>(0);
					PendingPublication = Ticket;
					(void)Tasks::LaunchTask("AssetRegistry.QueryEvents",
						[this, Ticket = std::move(Ticket)]() mutable { Ticket.reset(); Synchronize(); }, Tasks::ETaskThread::GameThread);
				}
			}
			FDelegateHandle PackagesChanged, CatalogChanged, MountsChanged;
			FAssetQueryState()
			{
				MountsChanged = FMountPaths::OnMountPointsChanged().AddLambda([this] { bMountsChanged = true; Invalidate(); });
				PackagesChanged = OnAssetPackagesChanged().AddLambda([this] { Invalidate(); });
				CatalogChanged = OnAssetCatalogChanged().AddLambda([this](const FContentChangeBatch& Batch) {
					{ std::lock_guard Lock(PublicationMutex); PendingCatalogChanges.bFullRefresh |= Batch.bFullRefresh;
					  PendingCatalogChanges.Changes.insert(PendingCatalogChanges.Changes.end(), Batch.Changes.begin(), Batch.Changes.end()); }
					Invalidate();
				});
			}
			~FAssetQueryState()
			{
				FMountPaths::OnMountPointsChanged().Remove(MountsChanged);
				OnAssetPackagesChanged().Remove(PackagesChanged);
				OnAssetCatalogChanged().Remove(CatalogChanged);
			}
			FAssetQuerySnapshot Snapshot;
			std::unordered_map<FObjectKey, FAssetData> Live;
			std::unordered_set<FPackagePath> LivePackages;
			std::vector<FContentChangeBatch> PendingEvents;
			bool Publishing = false;

			auto Synchronize() -> void
			{
				if (GIsGameThreadIdInitialized) CheckGameThread();
				const bool MountsChanged = std::exchange(bMountsChanged, false);
				if (!Dirty.exchange(false) && !MountsChanged) return;
				FContentChangeBatch CatalogChanges;
				{ std::lock_guard Lock(PublicationMutex); CatalogChanges = std::exchange(PendingCatalogChanges, {}); }

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

				FContentChangeBatch Batch{.bFullRefresh = CatalogChanges.bFullRefresh || MountsChanged};
				std::unordered_set<FPackagePath> DiskPackages;
				for (const auto& Change : CatalogChanges.Changes)
					for (const auto* Path : {&Change.OldAssetPath, &Change.NewAssetPath})
					{
						FPackagePath Package; if (FPackagePath::TryCreate(*Path, Package)) DiskPackages.insert(Package);
					}
				const auto Disk = CaptureAssetCatalogSnapshot();
				FAssetQuerySnapshot Next{.Packages = Disk.Assets};
				for (const auto& [Path, Package] : Next.Packages)
					if (!CurrentPackages.contains(Path)) for (const auto& Asset : Package.TopLevelAssets) Next.Assets.emplace(Asset.AssetPath, Asset);
				for (const auto& [Key, Asset] : Current) Next.Assets.insert_or_assign(Asset.AssetPath, Asset);
				auto Append = [&](EContentChangeKind Kind, const FAssetData* Before, const FAssetData* After) {
					Batch.Changes.push_back({Kind,
						Before ? PackageFilename(Before->AssetPath.GetPackagePath()) : std::string{},
						After ? PackageFilename(After->AssetPath.GetPackagePath()) : std::string{},
						Before ? Before->AssetPath.ToString() : std::string{}, After ? After->AssetPath.ToString() : std::string{}});
				};
				std::unordered_set<FTopLevelAssetPath> RenamedFrom, RenamedTo, ResidencyChanged;
				for (const auto& [Key, Before] : Live)
				{
					const auto Found = Current.find(Key);
					if (Found == Current.end()) ResidencyChanged.insert(Before.AssetPath);
					else if (Before.AssetPath != Found->second.AssetPath)
					{
						Append(EContentChangeKind::Renamed, &Before, &Found->second);
						RenamedFrom.insert(Before.AssetPath); RenamedTo.insert(Found->second.AssetPath);
					}
				}
				for (const auto& [Key, After] : Current) if (!Live.contains(Key)) ResidencyChanged.insert(After.AssetPath);
				for (const auto& [Path, Before] : Snapshot.Assets)
				{
					if (RenamedFrom.contains(Path)) continue;
					const auto* After = Next.FindAssetExact(Path);
					if (!After) Append(EContentChangeKind::Removed, &Before, nullptr);
					else if (Before != *After || ResidencyChanged.contains(Path) || DiskPackages.contains(Path.GetPackagePath()))
						Append(EContentChangeKind::Modified, &Before, After);
				}
				for (const auto& [Path, After] : Next.Assets)
					if (!RenamedTo.contains(Path) && !Snapshot.Assets.contains(Path)) Append(EContentChangeKind::Added, nullptr, &After);
				Live = std::move(Current); LivePackages = std::move(CurrentPackages);
				Snapshot = std::move(Next);
				if (Batch.Changes.empty() && !Batch.bFullRefresh) return;
				PendingEvents.push_back(std::move(Batch));
				if (!Publishing)
				{
					Publishing = true;
					struct FGuard { bool& Flag; ~FGuard() { Flag = false; } } Guard{Publishing};
					while (!PendingEvents.empty())
					{
						auto Event = std::move(PendingEvents.front()); PendingEvents.erase(PendingEvents.begin());
						OnAssetQueryChanged().Broadcast(Event);
					}
				}
			}
		};

		auto QueryState() -> FAssetQueryState&
		{
			static FAssetQueryState State;
			State.Synchronize();
			return State;
		}
	}

	auto OnAssetQueryChanged() -> TMulticastDelegate<void(const FContentChangeBatch&)>& { static TMulticastDelegate<void(const FContentChangeBatch&)> Event; return Event; }

	auto CaptureAssetQuerySnapshot(EAssetQuerySource Source) -> FAssetQuerySnapshot
	{
		if (Source == EAssetQuerySource::OnDisk)
		{
			auto Disk = CaptureAssetCatalogSnapshot();
			FAssetQuerySnapshot Result{.Packages = std::move(Disk.Assets)};
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
		FAssetCatalogEntry Result{.Revision = GetAssetCatalogRevision()};
		if (Asset) Result.Asset = *Asset;
		if (const auto* Package = State.Snapshot.FindPackageExact(Path.GetPackagePath())) Result.Package = *Package;
		return Result;
	}

}
