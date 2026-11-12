#include "Panels/ContentBrowserChanges.h"
#include "AssetRegistry/Scan.h"
#include "ContentBrowserNativeDataSource.h"
#include "Data/ContentBrowserNativeUpdates.h"
#include "Operations/ContentBrowserPaths.h"
#include "Operations/ContentBrowserPathInspection.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/RegistryOperations.h"
#include "AssetTools/AssetDeletion.h"
#include "Threading/Task.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::ContentBrowser::Private
{
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
		uint64 Generation = 0, Mount = 0, Epoch = 0, CapturedEpoch = 0, Revision = 1;
		FContentChangeJournal Changes;
		std::string Project;
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
	using namespace ContentBrowser::Private;
	auto DContentBrowserAssetDataSource::Initialize(bool AutoRegister) -> void
	{
		if (IsInitialized()) return;
		Super::Initialize(AutoRegister);
		QueryChanged = OnAssetQueryChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserAssetDataSource>(this)](const FContentChangeBatch& Batch) {
			if (auto* Source = Weak.Get(); Source && Source->IsActive()) { TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source); QueueNativeCommittedUpdates(*Source, Batch, true); }
		});
	}
	auto DContentBrowserAssetDataSource::Shutdown() -> void
	{
		OnAssetQueryChanged().Remove(QueryChanged); QueryChanged = {};
		DiscoveryCatalog.reset(); Super::Shutdown();
	}
	auto DContentBrowserAssetDataSource::Tick(float) -> void { if (IsActive()) (void)GetAssetQueryRevision(); }
	auto DContentBrowserFileDataSource::Initialize(bool AutoRegister) -> void
	{
		if (IsInitialized()) return;
		Super::Initialize(AutoRegister);
		Updates = std::make_shared<FContentBrowserFileUpdateState>();
	}
	auto DContentBrowserFileDataSource::Shutdown() -> void
	{
		if (Updates) Updates->Cancel(); Updates.reset(); Super::Shutdown();
	}
	auto RecordContentBrowserFileUpdates(DContentBrowserFileDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
	{
		if (!Source.Updates || (!Batch.bFullRefresh && Batch.Changes.empty())) return;
		auto& State = *Source.Updates;
		auto Record = Batch; Record.FromRevision = State.Revision; Record.ToRevision = ++State.Revision;
		State.Changes.Append(std::move(Record));
		if (!FromDiscovery) { ++State.Epoch; State.Baseline = true; State.Elapsed = 1.0f; }
	}
	auto DContentBrowserFileDataSource::GetDiscoveryRevision() const -> uint64 { return Updates ? Updates->Revision : 0; }
	auto DContentBrowserFileDataSource::IsDiscoveryPathAffected(uint64 Since, std::string_view Path, bool Recursive) const -> bool
	{
		if (!Updates) return true;
		const auto Batch = Updates->Changes.Read(Since, Updates->Revision);
		if (Batch.bFullRefresh) return true;
		const auto Physical = FContentBrowserPaths::VirtualToPhysical(Path);
		return std::ranges::any_of(Batch.Changes, [&](const auto& Change) { return ContentBrowserChanges::Affects(Change, Physical, Recursive) || !ContentBrowserChanges::IntroducedChild(Change, Physical).empty(); });
	}
	auto DContentBrowserFileDataSource::Tick(float DeltaSeconds) -> void
	{
		if (!IsActive() || !Updates) return;
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto State = Updates;
		const auto Generation = GetActivationGeneration(), Mount = FMountPaths::GetMountRegistryRevision();
		const auto Project = FPaths::ProjectFile();
		if (State->Generation != Generation || State->Mount != Mount || State->Project != Project)
		{
			State->Cancel(); State->Previous = {}; State->Baseline = true; State->Elapsed = 1.0f;
			State->Generation = Generation; State->Mount = Mount; State->Project = Project;
		}
		if (State->Task.IsValid() && State->Task.IsCompleted())
		{
			if (State->Task.GetState() == ETaskState::Succeeded && State->CapturedEpoch == State->Epoch)
			{
				auto Snapshot = std::move(State->Task.GetResult());
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
				if (PackagesChanged) { (void)RefreshAssetRegistry(EAssetRegistryScanMode::Incremental); (void)GetAssetQueryRevision(); }
				if (IsActive() && Updates == State && State->CapturedEpoch == State->Epoch && GetActivationGeneration() == Generation && FMountPaths::GetMountRegistryRevision() == Mount && FPaths::ProjectFile() == Project) QueueNativeCommittedUpdates(*this, Batch, true);
			}
			State->Task = {}; State->Cancellation.reset();
		}
		if (!IsActive() || Updates != State || GetActivationGeneration() != Generation || FMountPaths::GetMountRegistryRevision() != Mount || FPaths::ProjectFile() != Project) return;
		State->Elapsed += std::max(DeltaSeconds, 0.0f);
		if (!State->Task.IsValid() && State->Elapsed >= 0.5f && IsActive() && IsTaskSchedulerRunning())
		{
			std::vector<std::string> Roots;
			for (const auto& Root : FContentBrowserPaths::CaptureMounts()) Roots.push_back(Root.PhysicalRoot);
			State->Cancellation = std::make_shared<Tasks::FCancellationToken>();
			State->CapturedEpoch = State->Epoch; State->Elapsed = 0.0f;
			State->Task = Tasks::LaunchTask("ContentBrowser.FileUpdates", [Roots = std::move(Roots), Cancellation = State->Cancellation] { return CaptureFiles(Roots, *Cancellation); });
		}
	}
}
