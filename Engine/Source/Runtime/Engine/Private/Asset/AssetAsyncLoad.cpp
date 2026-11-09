#include "Asset/StreamableHandle.h"
#include "PackageLoadTransaction.h"
#include "AssetAsyncLoadInternal.h"
#include "AssetObjectLoadTiming.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetLiveLoadGuard.h"
#include "Asset/Redirector.h"
#include "Asset/RegistryOperations.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/FileHelper.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace
	{
		auto CheckAsyncLoadThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		// Bound detached data independently of scheduler queue capacity.
		constexpr uint64 MaximumClosureBytes = 256ull * 1024 * 1024;
		constexpr size_t MaximumConcurrentReads = 2;
		auto ReadInputs(std::vector<FPackageData> Sources, const Tasks::FCancellationToken& Token)
			-> FAsyncPackageInputs
		{
			FAsyncPackageInputs Inputs;
			uint64 RetainedBytes = 0;
			for (const auto& Source : Sources)
			{
				if (Token.IsCanceled()) break;
				auto& Input = Inputs[Source.PackagePath];
				Input.PhysicalPath = Source.PhysicalPath;
				Input.Access = FPackageFileAccess::TryReadPackage(Input.PhysicalPath);
				if (!Input.Access)
				{
					Input.Result = {EAssetReadError::InUse, "Package output is being written."};
					continue;
				}
				std::error_code Error;
				const auto Size = std::filesystem::file_size(Input.PhysicalPath, Error);
				if (Error)
					Input.Result = {Error == std::errc::no_such_file_or_directory
						? EAssetReadError::NotFound : EAssetReadError::IoError, Error.message()};
				else if (Size > MaximumClosureBytes - RetainedBytes)
					Input.Result = {EAssetReadError::InUse, "Async package closure exceeds the 256 MiB read budget."};
				else
				{
					auto Loaded = FFileHelper::LoadFileToArray(Input.PhysicalPath);
					if (!Loaded)
						Input.Result = {EAssetReadError::IoError, Loaded.error().ToString()};
					else if (Loaded->size() > MaximumClosureBytes - RetainedBytes)
						Input.Result = {EAssetReadError::StaleData, "Package size changed during async read."};
					else Input.Bytes = std::move(*Loaded);
				}
				RetainedBytes += Input.Bytes.size();
				if (!Input.Result) continue;
				auto Bulk = std::filesystem::path(Input.PhysicalPath);
				Bulk.replace_extension(".dbulk");
				if (std::filesystem::is_regular_file(Bulk, Error))
					Input.BulkBytes = std::filesystem::file_size(Bulk, Error);
				if (Error && Error != std::errc::no_such_file_or_directory)
					Input.Result = {EAssetReadError::IoError, "Failed to inspect async bulk companion."};
			}
			return Inputs;
		}
	}

	struct FStreamableHandle::FImpl
	{
		EAssetLoadState State = EAssetLoadState::Pending;
		std::optional<FAssetReadError> Failure;
		FAssetLoadReport Report;
		FPackagePath PackagePath;
		std::optional<FObjectPath> ObjectPath;
		const DClass* ExpectedClass = nullptr;
		int32 Priority = 0;
		FStreamableDelegate Callback;
		TStrongObjectPtr<DPackage> Package;
		TStrongObjectPtr<DObject> Object;
		TWeakObjectPtr<DPackage> LoadedPackage;
		TWeakObjectPtr<DObject> LoadedObject;
		bool bReleaseRequested = false;
		bool bReleased = false;
		bool bCompletionPending = false;
	};

	FStreamableHandle::FStreamableHandle() : Impl(std::make_unique<FImpl>()) {}
	FStreamableHandle::~FStreamableHandle() { CheckAsyncLoadThread(); ReleaseRetention(); }
	auto FStreamableHandle::GetState() const -> EAssetLoadState { CheckAsyncLoadThread(); return Impl->State; }
	auto FStreamableHandle::IsComplete() const -> bool
	{
		const auto State = GetState();
		return State == EAssetLoadState::Succeeded || State == EAssetLoadState::Failed || State == EAssetLoadState::Cancelled;
	}
	auto FStreamableHandle::IsActive() const -> bool
	{
		CheckAsyncLoadThread();
		return !Impl->bReleased && (Impl->State == EAssetLoadState::Pending
			|| Impl->State == EAssetLoadState::Loading || Impl->State == EAssetLoadState::Succeeded);
	}
	auto FStreamableHandle::GetResult() const -> std::expected<FStreamableLoadResult, FAssetReadError>
	{
		CheckAsyncLoadThread();
		check(IsComplete());
		if (Impl->Failure) return std::unexpected(*Impl->Failure);
		return FStreamableLoadResult{GetLoadedPackage(), GetLoadedObject()};
	}
	auto FStreamableHandle::GetReport() const -> const FAssetLoadReport& { CheckAsyncLoadThread(); return Impl->Report; }
	auto FStreamableHandle::GetLoadedPackage() const -> DPackage*
	{
		CheckAsyncLoadThread();
		return Impl->bReleased ? Impl->LoadedPackage.Get() : Impl->Package.Get();
	}
	auto FStreamableHandle::GetLoadedObject() const -> DObject*
	{
		CheckAsyncLoadThread();
		return Impl->bReleased ? Impl->LoadedObject.Get() : Impl->Object.Get();
	}
	auto FStreamableHandle::ReleaseRetention() -> void
	{
		if (Impl->bReleased) return;
		const bool bHadRetention = Impl->Package.IsValid() || Impl->Object.IsValid();
		Impl->LoadedPackage = Impl->Package.Get();
		Impl->LoadedObject = Impl->Object.Get();
		Impl->Object.Reset();
		Impl->Package.Reset();
		Impl->bReleased = true;
		if (bHadRetention) RequestGarbageCollection();
	}
	auto FStreamableHandle::ReleaseHandle() -> void
	{
		CheckAsyncLoadThread();
		Impl->bReleaseRequested = true;
		if (IsComplete() && !Impl->bCompletionPending) ReleaseRetention();
	}
	auto FStreamableHandle::CancelHandle() -> void
	{
		CheckAsyncLoadThread();
		Impl->Callback = {};
		Impl->bCompletionPending = false;
		Impl->bReleaseRequested = true;
		if (!IsComplete())
		{
			Impl->State = EAssetLoadState::Cancelled;
			Impl->Failure = FAssetReadError{EAssetReadError::Cancelled, "Asset load request cancelled."};

		}
		ReleaseRetention();
	}

	class FAssetLoadRequestService
	{
		struct FJob
		{
			FPackagePath Path;
			uint64 Revision = 0;
			std::vector<std::shared_ptr<FStreamableHandle>> Handles;
			Tasks::TTask<FAsyncPackageInputs> Read;
			Tasks::FCancellationToken Cancellation;
		};
		std::vector<std::shared_ptr<FStreamableHandle>> Pending;
		std::vector<std::shared_ptr<FStreamableHandle>> Deliveries;
		std::vector<std::shared_ptr<FJob>> Jobs;
		bool bProcessing = false;

		auto Complete(const std::shared_ptr<FStreamableHandle>& Handle, FAssetReadResult Result,
			DPackage* Package = nullptr, DObject* Object = nullptr) -> void
		{
			if (Handle->IsComplete()) return;
			auto& State = *Handle->Impl;

			State.State = Result ? EAssetLoadState::Succeeded : EAssetLoadState::Failed;
			if (Result)
			{
				State.Package = Package; State.Object = Object;
				State.LoadedPackage = Package; State.LoadedObject = Object;
			}
			else State.Failure = AssetReadErrorFromResult(Result);
			State.bCompletionPending = static_cast<bool>(State.Callback);
			if (State.Callback) Deliveries.push_back(Handle);
			else if (State.bReleaseRequested) Handle->ReleaseRetention();
		}

		auto Apply(const std::shared_ptr<FStreamableHandle>& Handle, const FAsyncPackageInputs* Inputs = nullptr) -> void
		{
			if (Handle->IsComplete()) return;
			AssetPrivate::FObjectLoadTimingSession TimingSession(Handle->Impl->PackagePath);
			AssetPrivate::FScopedObjectLoadPhase ApplyTiming(Handle->Impl->PackagePath, AssetPrivate::EObjectLoadPhase::ObjectApplication);
			auto& Loader = FAssetRuntimeState::Get().GetLoadService();
			auto& State = *Handle->Impl;
			DPackage* Package = nullptr;
			DObject* Object = nullptr;
			FAssetReadResult Result;
			FPackageLoadTransaction LoadScope;
			{
				// Preserve the synchronous loader's SCC ordering, rollback and publication.
				// No uncaptured dependency may silently perform disk I/O on GameThread.
				const auto* Previous = std::exchange(Loader.AsyncInputs, Inputs);
				struct FRestore { decltype(Loader.AsyncInputs)& Slot; decltype(Previous) Value;
					~FRestore() { Slot = Value; } } Restore{Loader.AsyncInputs, Previous};
				if (State.ObjectPath)
				{
					auto Loaded = LoadScope.LoadObject(*State.ObjectPath, State.ExpectedClass, &State.Report);
					Object = Loaded.value_or(nullptr);
					if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
					Package = Object ? Object->GetPackage() : nullptr;
				}
				else
				{
					auto Loaded = LoadScope.LoadPackage(State.PackagePath, &State.Report);
					Package = Loaded.value_or(nullptr);
					if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
				}
			}
			Complete(Handle, std::move(Result), Package, Object);
			(void)LoadScope.ReleaseCache();
		}

	public:
		auto Request(FPackagePath PackagePath, std::optional<FObjectPath> ObjectPath,
			const DClass* ExpectedClass, FStreamableDelegate Callback, int32 Priority)
			-> std::shared_ptr<FStreamableHandle>
		{
			CheckAsyncLoadThread();
			auto Handle = std::shared_ptr<FStreamableHandle>(new FStreamableHandle());
			auto& State = *Handle->Impl;
			State.PackagePath = std::move(PackagePath);
			State.ObjectPath = std::move(ObjectPath);
			State.ExpectedClass = ExpectedClass;
			State.Callback = std::move(Callback);
			State.Priority = Priority;
			State.Report.RequestedPath = State.PackagePath;
			// Rejected lifetime/guard admissions must never enqueue work for later escape.
			auto Admission = AssetPrivate::FAssetLiveLoadGuard::Check("async load", State.PackagePath.ToString());
			if (Admission && !FAssetRuntimeState::Get().IsAcceptingRequests())
				Admission = {EAssetReadError::ShuttingDown, "Asset loading is closed."};
			if (!Admission)
			{
				State.Callback = {};
				Complete(Handle, std::move(Admission));
			}
			else Pending.push_back(Handle);
			return Handle;
		}

		auto RequestSync(FPackagePath PackagePath, std::optional<FObjectPath> ObjectPath,
			const DClass* ExpectedClass) -> std::shared_ptr<FStreamableHandle>
		{
			CheckAsyncLoadThread();
			auto Handle = std::shared_ptr<FStreamableHandle>(new FStreamableHandle());
			Handle->Impl->PackagePath = std::move(PackagePath);
			Handle->Impl->ObjectPath = std::move(ObjectPath);
			Handle->Impl->ExpectedClass = ExpectedClass;
			Handle->Impl->Report.RequestedPath = Handle->Impl->PackagePath;
			auto Admission = AssetPrivate::FAssetLiveLoadGuard::Check("sync load", Handle->Impl->PackagePath.ToString());
			if (Admission && !FAssetRuntimeState::Get().IsAcceptingRequests())
				Admission = {EAssetReadError::ShuttingDown, "Asset loading is closed."};
			if (Admission && (!Handle->Impl->PackagePath.IsValid()
				|| (Handle->Impl->ObjectPath && !Handle->Impl->ObjectPath->IsValid())))
				Admission = {EAssetReadError::InvalidPath, "A load request requires a valid package or object path."};
			if (Admission) Apply(Handle);
			else Complete(Handle, std::move(Admission));
			return Handle;
		}

		auto Process(double Milliseconds, uint32 MaximumCompletions) -> void
		{
			CheckAsyncLoadThread();
			if (bProcessing || MaximumCompletions == 0) return;
			if (!AssetPrivate::FAssetLiveLoadGuard::Check("async load pump", "")) return;
			bProcessing = true;
			struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bProcessing};
			const auto Start = std::chrono::steady_clock::now();
			uint32 Completed = 0;
			const FAsyncPackageInputs EmptyInputs;
			auto HasBudget = [&] {
				return Completed < MaximumCompletions && (Completed == 0 ||
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count() < Milliseconds);
			};
			// Deliver only after the previous pump released input read leases. Callbacks
			// may save, enqueue, cancel peers, or shut down the manager.
			while (!Deliveries.empty() && HasBudget())
			{
				auto Handle = std::move(Deliveries.front());
				Deliveries.erase(Deliveries.begin());
				auto Callback = std::move(Handle->Impl->Callback);
				Handle->Impl->bCompletionPending = false;
				if (Callback)
				{
					try { Callback(*Handle); }
					catch (...) { DURIN_ERROR("Async asset load completion callback threw an exception."); }
					++Completed;
				}
				if (Handle->Impl->bReleaseRequested) Handle->ReleaseRetention();
			}
			for (size_t Index = 0; Index < Jobs.size() && HasBudget();)
			{
				auto& Job = *Jobs[Index];
				if (std::ranges::all_of(Job.Handles, [](const auto& H) { return H->IsComplete(); }))
					Job.Cancellation.Cancel();
				if (!Job.Read.IsCompleted()) { ++Index; continue; }
				auto Finished = std::move(Jobs[Index]);
				Jobs.erase(Jobs.begin() + Index);
				FAssetReadResult Result;
				FAsyncPackageInputs Inputs;
				if (Finished->Cancellation.IsCanceled())
					Result = {EAssetReadError::Cancelled, "Async package read cancellation was requested."};
				else if (Finished->Read.GetState() != ETaskState::Succeeded)
					Result = {Finished->Read.GetState() == ETaskState::Canceled
						? EAssetReadError::Cancelled : EAssetReadError::IoError, "Async package read did not succeed."};
				else Inputs = std::move(Finished->Read.GetResult());
				if (Result && Finished->Revision != GetAssetCatalogRevision())
					Result = {EAssetReadError::StaleData, "Asset catalog changed during async loading; retry the request."};
				for (auto& Handle : Finished->Handles)
				{
					if (Result) Apply(Handle, &Inputs);
					else Complete(Handle, Result);
				}
				++Completed;
			}
			// Snapshot requests so callbacks can safely enqueue the next generation.
			auto Requests = std::exchange(Pending, {});
			std::stable_sort(Requests.begin(), Requests.end(), [](const auto& A, const auto& B) {
				return A->Impl->Priority > B->Impl->Priority;
			});
			for (auto& Handle : Requests)
			{
				if (Handle->IsComplete()) continue;
				if (!HasBudget()) { Pending.push_back(Handle); continue; }
				auto& State = *Handle->Impl;
				if (!FAssetRuntimeState::Get().IsAcceptingRequests())
				{
					Complete(Handle, {EAssetReadError::ShuttingDown, "Asset loading is closed."});
					++Completed; continue;
				}
				FPackagePath Root = State.PackagePath;
				if (State.ObjectPath)
				{
					const auto Resolution = ResolveAssetObjectPathForOperation(*State.ObjectPath,
						{.ExpectedClass = State.ExpectedClass});
					if (!Resolution) { Apply(Handle, &EmptyInputs); ++Completed; continue; }
					Root = Resolution.FinalPath.GetPackagePath();
				}
				if (!Root.IsValid())
				{
					Complete(Handle, {EAssetReadError::InvalidPath, "Async load requires a valid package path."});
					++Completed; continue;
				}
				if (FindResidentPackage(Root)) { Apply(Handle, &EmptyInputs); ++Completed; continue; }
				const auto Existing = std::ranges::find_if(Jobs, [&](const auto& Job) { return Job->Path == Root; });
				if (Existing != Jobs.end() && !(*Existing)->Cancellation.IsCanceled())
				{
					(*Existing)->Handles.push_back(Handle);
					State.State = EAssetLoadState::Loading;
					continue;
				}
				if (Jobs.size() >= MaximumConcurrentReads) { Pending.push_back(Handle); continue; }
				if (!IsTaskSchedulerRunning())
				{
					Complete(Handle, {EAssetReadError::InUse, "Async loading requires a running task scheduler."});
					++Completed; continue;
				}
				auto Job = std::make_shared<FJob>();
				Job->Path = Root;
				Job->Revision = GetAssetCatalogRevision();
				Job->Handles.push_back(Handle);
				std::vector<FPackageData> Sources;
				std::vector<FPackagePath> Paths{Root};
				std::unordered_set<FPackagePath> Visited;
				while (!Paths.empty())
				{
					const auto Path = Paths.back(); Paths.pop_back();
					if (!Visited.insert(Path).second || FindResidentPackage(Path)) continue;
					const auto Entry = FindPackageExact(Path);
					if (!Entry) continue; // Only the live serializer decides which dependencies are needed.
					Paths.insert(Paths.end(), Entry->Dependencies.begin(), Entry->Dependencies.end());
					for (const auto& Asset : Entry->TopLevelAssets)
						if (Asset.IsRedirector()) Paths.push_back(Asset.RedirectDestination.GetPackagePath());
					Sources.push_back(*Entry);
				}
				Job->Read = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO,
					{.DebugName = "Asset.PackageRead"},
					[Sources = std::move(Sources), Job]() mutable {
						return ReadInputs(std::move(Sources), Job->Cancellation);
					});
				State.State = EAssetLoadState::Loading;
				Jobs.push_back(std::move(Job));
			}
		}

		auto Shutdown() -> void
		{
			CheckAsyncLoadThread();
			if (!AssetPrivate::FAssetLiveLoadGuard::Check("async load cancellation", "")) return;
			for (auto& Handle : Pending) Handle->CancelHandle();
			Pending.clear();
			for (auto& Handle : Deliveries) Handle->CancelHandle();
			Deliveries.clear();
			for (auto& Job : Jobs)
			{
				for (auto& Handle : Job->Handles) Handle->CancelHandle();
				Job->Cancellation.Cancel();
				(void)WaitTask(Durin::Tasks::FTask(Job->Read));
			}
			Jobs.clear();
		}
	};

	namespace { auto AsyncLoader() -> FAssetLoadRequestService& { static FAssetLoadRequestService Loader; return Loader; } }
	auto RequestSyncLoad(const FPackagePath& Path) -> std::shared_ptr<FStreamableHandle>
	{
		return AsyncLoader().RequestSync(Path, {}, nullptr);
	}
	auto RequestSyncLoad(const FObjectPath& Path, const DClass* ExpectedClass) -> std::shared_ptr<FStreamableHandle>
	{
		return AsyncLoader().RequestSync(Path.GetPackagePath(), Path, ExpectedClass);
	}
	auto RequestSyncLoad(const FTopLevelAssetPath& Path, const DClass* ExpectedClass) -> std::shared_ptr<FStreamableHandle>
	{
		FObjectPath ObjectPath;
		FObjectPath::TryCreate(Path, std::span<const std::string>{}, ObjectPath);
		return RequestSyncLoad(ObjectPath, ExpectedClass);
	}
	auto RequestAsyncLoad(const FPackagePath& Path, FStreamableDelegate Callback, int32 Priority)
		-> std::shared_ptr<FStreamableHandle>
	{
		return AsyncLoader().Request(Path, {}, nullptr, std::move(Callback), Priority);
	}
	auto RequestAsyncLoad(const FObjectPath& Path, FStreamableDelegate Callback,
		const DClass* ExpectedClass, int32 Priority) -> std::shared_ptr<FStreamableHandle>
	{
		return AsyncLoader().Request(Path.GetPackagePath(), Path, ExpectedClass, std::move(Callback), Priority);
	}
	auto RequestAsyncLoad(const FTopLevelAssetPath& Path, FStreamableDelegate Callback,
		const DClass* ExpectedClass, int32 Priority) -> std::shared_ptr<FStreamableHandle>
	{
		FObjectPath ObjectPath;
		FObjectPath::TryCreate(Path, std::span<const std::string>{}, ObjectPath);
		return RequestAsyncLoad(ObjectPath, std::move(Callback), ExpectedClass, Priority);
	}
	auto ProcessAsyncLoading(double Milliseconds, uint32 MaximumCompletions) -> void { AsyncLoader().Process(Milliseconds, MaximumCompletions); }
	auto CancelAsyncLoading() -> void { AsyncLoader().Shutdown(); }
}
