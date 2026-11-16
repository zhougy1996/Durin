#include "AssetTools/ContentMutation.h"
#include "AssetRegistry/ContentChangeScopes.h"
#include "AssetRegistry/Scan.h"
#include "ContentBrowserFileDataSource.h"
#include "Data/ContentBrowserNativeUpdates.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/ContentPathInspection.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/RegistryOperations.h"
#include "AssetTools/AssetDeleteExtensions.h"
#include "Threading/Task.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::ContentBrowser::FileDataSource::Private
{
	using ContentStorage::FindReparsePointInPath;
	using ContentStorage::AreSamePath;
	struct FFileStamp
	{
		bool Directory = false;
		uintmax_t Size = 0;
		std::filesystem::file_time_type Time;
		auto operator==(const FFileStamp&) const -> bool = default;
	};
	struct FFileSnapshot
	{
		std::map<std::string, FFileStamp> Files;
		bool Complete = true;
	};
	struct FContentBrowserFileUpdateState
	{
		Tasks::TTask<FFileSnapshot> Task;
		std::shared_ptr<Tasks::FCancellationToken> Cancellation;
		FFileSnapshot Previous;
		float Elapsed = 1.0f;
		bool Baseline = true;
		auto Cancel() -> void
		{
			if (Cancellation) Cancellation->Cancel();
			if (Task.IsValid()) (void)Task.Wait();
			Task = {}; Cancellation.reset();
		}
	};
	static auto CaptureFiles(const std::vector<std::string>& Roots, const Tasks::FCancellationToken& Cancellation) -> FFileSnapshot
	{
		FFileSnapshot Result;
		for (const auto& Root : Roots)
		{
			std::error_code Error;
			std::filesystem::recursive_directory_iterator It(Root, Error), End;
			if (Error) Result.Complete = false;
			while (!Error && It != End && !Cancellation.IsCanceled())
			{
				const auto Path = It->path();
				const bool NestedMount = std::ranges::any_of(Roots, [&](const auto& Other) { return Other != Root && AreSamePath(Path.generic_string(), Other); });
				std::filesystem::path Reparse;
				const bool Unsafe = FindReparsePointInPath(Root, Path.generic_string(), Reparse, Error);
				if (NestedMount || Unsafe) It.disable_recursion_pending();
				else if (!Error)
				{
					const auto Status = It->symlink_status(Error);
					if (!Error && (std::filesystem::is_regular_file(Status) || std::filesystem::is_directory(Status)))
					{
						FFileStamp Stamp{.Directory = std::filesystem::is_directory(Status)};
						Stamp.Time = It->last_write_time(Error);
						if (!Error && !Stamp.Directory) Stamp.Size = It->file_size(Error);
						if (!Error) Result.Files.emplace(Path.lexically_normal().generic_string(), Stamp);
					}
				}
				if (Error) { Result.Complete = false; break; }
				It.increment(Error);
			}
			if (Error || Cancellation.IsCanceled()) Result.Complete = false;
		}
		return Result;
	}
}
namespace Durin
{
	using namespace ContentBrowser::FileDataSource::Private;



	auto DContentBrowserFileDataSource::Initialize(bool AutoRegister) -> void
	{
		if (IsInitialized()) return;
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		Super::Initialize(false);
		DirectoriesChanged = ContentStorage::OnContentDirectoriesChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserFileDataSource>(this)](const FContentChangeBatch& Batch, const DObject* Origin, ContentStorage::EContentDirectoryChangeReason Reason) {
			const auto* OriginSource = Cast<DContentBrowserDataSource>(Origin);
			if (auto* Source = Weak.Get(); Source && OriginSource && Source != OriginSource && Source->GetOwningEditor()
				&& Source->GetOwningEditor() == OriginSource->GetOwningEditor() && Source->IsActive())
			{
				TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
				QueueNativeCommittedUpdates(*Source, Batch, Reason == ContentStorage::EContentDirectoryChangeReason::Discovery);
			}
		});
		MountsChanged = FMountPaths::OnMountPointsChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserFileDataSource>(this)] {
			if (auto* Source = Weak.Get()) Source->ResetDiscovery();
		});
		ResetDiscovery();
		if (AutoRegister) RegisterDataSource();
	}
	auto DContentBrowserFileDataSource::Shutdown() -> void
	{
		FMountPaths::OnMountPointsChanged().Remove(MountsChanged); MountsChanged = {};
		ContentStorage::OnContentDirectoriesChanged().Remove(DirectoriesChanged); DirectoriesChanged = {};
		if (Updates) Updates->Cancel(); Updates.reset(); Super::Shutdown();
	}
	auto DContentBrowserFileDataSource::ResetDiscovery() -> void
	{
		if (Updates) Updates->Cancel();
		Updates = std::make_shared<FContentBrowserFileUpdateState>();
	}
	auto RecordContentBrowserFileUpdates(DContentBrowserFileDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
	{
		if (!Source.Updates || (!Batch.bFullRefresh && Batch.Changes.empty())) return;
		if (!FromDiscovery) Source.ResetDiscovery();
	}
	auto DContentBrowserFileDataSource::Tick(float DeltaSeconds) -> void
	{
		if (!IsActive() || !Updates) return;
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto State = Updates;
		if (State->Task.IsValid() && State->Task.IsCompleted())
		{
			auto Completed = std::exchange(State->Task, {});
			State->Cancellation.reset();
			if (Completed.GetState() == ETaskState::Succeeded)
			{
				auto Snapshot = std::move(Completed.GetResult());
				FContentChangeBatch Batch;
				bool PackagesChanged = false;
				auto Append = [&](EContentChangeKind Kind, const std::string& Path, bool Directory) {
					const auto Extension = std::filesystem::path(Path).extension().string();
					if (Extension == ".dasset" || Extension == ".dbulk") { PackagesChanged = true; return; }
					if (!Directory)
					{
						FAssetCompanionOwnership Ownership;
						if (!QueryAssetCompanionOwnership(Path, Ownership) || Ownership.State != EAssetCompanionOwnershipState::Unclaimed) return;
					}
					Batch.Changes.push_back({Kind, Kind == EContentChangeKind::Removed ? Path : std::string{}, Kind == EContentChangeKind::Removed ? std::string{} : Path, {}, {}, Directory});
				};
				if (Snapshot.Complete && !State->Baseline)
				{
					for (const auto& [Path, Stamp] : State->Previous.Files)
						if (!Snapshot.Files.contains(Path)) Append(EContentChangeKind::Removed, Path, Stamp.Directory);
					for (const auto& [Path, Stamp] : Snapshot.Files)
					{
						const auto Found = State->Previous.Files.find(Path);
						if (Found == State->Previous.Files.end()) Append(EContentChangeKind::Added, Path, Stamp.Directory);
						else if (Found->second != Stamp) Append(EContentChangeKind::Modified, Path, Stamp.Directory);
					}
				}
				else { Batch.bFullRefresh = true; PackagesChanged = true; }
				if (Snapshot.Complete) { State->Previous = std::move(Snapshot); State->Baseline = false; }
				if (PackagesChanged) { (void)RefreshAssetRegistry(EAssetRegistryScanMode::Incremental); }
				if (Updates == State && IsActive())
				{ QueueNativeCommittedUpdates(*this, Batch, true); ContentStorage::PublishContentDirectoryChanges(Batch, this, ContentStorage::EContentDirectoryChangeReason::Discovery); }
			}
		}
		if (Updates != State || !IsActive()) return;
		State->Elapsed += std::max(DeltaSeconds, 0.0f);
		if (!State->Task.IsValid() && State->Elapsed >= 0.5f && IsActive() && IsTaskSchedulerRunning())
		{
			std::vector<std::string> Roots;
			for (const auto& Root : ContentStorage::FContentMountPaths::CaptureMounts()) Roots.push_back(Root.PhysicalRoot);
			State->Cancellation = std::make_shared<Tasks::FCancellationToken>();
			State->Elapsed = 0.0f;
			State->Task = Tasks::LaunchTask("ContentBrowser.FileUpdates", [Roots = std::move(Roots), Cancellation = State->Cancellation] { return CaptureFiles(Roots, *Cancellation); });
		}
	}
}
