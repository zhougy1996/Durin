#include "Asset/AsyncLoad.h"
#include "AssetAsyncLoadInternal.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetLiveLoadGuard.h"
#include "Asset/Redirector.h"
#include "Asset/RegistryOperations.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"

#include <chrono>
#include <unordered_set>

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
		auto ReadInputs(std::vector<FAssetData> Sources, const FTaskCancellationToken& Token)
			-> FAsyncPackageInputs
		{
			FAsyncPackageInputs Inputs;
			uint64 RetainedBytes = 0;
			for (const auto& Source : Sources)
			{
				if (Token.IsCancellationRequested()) break;
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

	struct FAsyncLoadHandle::FImpl
	{
		EAsyncLoadState State = EAsyncLoadState::Pending;
		std::optional<std::expected<FAsyncLoadedAssets, FAssetReadError>> Result;
		FAssetLoadReport Report;
		FPackagePath PackagePath;
		std::optional<FObjectPath> ObjectPath;
		const DClass* ExpectedClass = nullptr;
		int32 Priority = 0;
		FAsyncLoadCallback Callback;
		TStrongObjectPtr<DPackage> Package;
		TStrongObjectPtr<DObject> Object;
	};

	FAsyncLoadHandle::FAsyncLoadHandle() : Impl(std::make_unique<FImpl>()) {}
	FAsyncLoadHandle::~FAsyncLoadHandle() { CheckAsyncLoadThread(); }
	auto FAsyncLoadHandle::GetState() const -> EAsyncLoadState { CheckAsyncLoadThread(); return Impl->State; }
	auto FAsyncLoadHandle::IsComplete() const -> bool
	{
		const auto State = GetState();
		return State == EAsyncLoadState::Succeeded || State == EAsyncLoadState::Failed
			|| State == EAsyncLoadState::Cancelled;
	}
	auto FAsyncLoadHandle::GetResult() const -> const std::expected<FAsyncLoadedAssets, FAssetReadError>&
	{
		CheckAsyncLoadThread();
		check(IsComplete() && Impl->Result);
		return *Impl->Result;
	}
	auto FAsyncLoadHandle::GetReport() const -> const FAssetLoadReport& { CheckAsyncLoadThread(); return Impl->Report; }
	auto FAsyncLoadHandle::GetLoadedPackage() const -> DPackage* { CheckAsyncLoadThread(); return Impl->Package.Get(); }
	auto FAsyncLoadHandle::GetLoadedObject() const -> DObject* { CheckAsyncLoadThread(); return Impl->Object.Get(); }
	auto FAsyncLoadHandle::Cancel() -> void
	{
		CheckAsyncLoadThread();
		if (IsComplete()) { Impl->Callback = {}; return; }
		Impl->State = EAsyncLoadState::Cancelled;
		Impl->Result = std::unexpected(FAssetReadError{EAssetReadError::Cancelled, "Async load request cancelled."});
		Impl->Report.Error = Impl->Result->error().Code;
		Impl->Report.ErrorMessage = Impl->Result->error().Message;
		Impl->Callback = {};
	}

	class FAsyncAssetLoadService
	{
		struct FJob
		{
			FPackagePath Path;
			uint64 Revision = 0;
			std::vector<std::shared_ptr<FAsyncLoadHandle>> Handles;
			Tasks::TTask<FAsyncPackageInputs> Read;
			FTaskCancellationSource Cancellation;
		};
		std::vector<std::shared_ptr<FAsyncLoadHandle>> Pending;
		std::vector<std::shared_ptr<FAsyncLoadHandle>> Deliveries;
		std::vector<std::unique_ptr<FJob>> Jobs;
		std::unique_ptr<Tasks::FTaskGroup> Group;
		bool bProcessing = false;

		auto Complete(const std::shared_ptr<FAsyncLoadHandle>& Handle, FAssetReadResult Result,
			DPackage* Package = nullptr, DObject* Object = nullptr) -> void
		{
			if (Handle->IsComplete()) return;
			auto& State = *Handle->Impl;
			State.Report.Error = Result.Error;
			State.Report.ErrorMessage = Result.Message;
			State.State = Result ? EAsyncLoadState::Succeeded : EAsyncLoadState::Failed;
			if (Result)
			{
				State.Package = Package; State.Object = Object;
				State.Result = FAsyncLoadedAssets{Package, Object};
			}
			else State.Result = std::unexpected(AssetReadErrorFromResult(Result));
			if (State.Callback) Deliveries.push_back(Handle);
		}

		auto Apply(const std::shared_ptr<FAsyncLoadHandle>& Handle, const FAsyncPackageInputs& Inputs) -> void
		{
			if (Handle->IsComplete()) return;
			auto& Loader = FAssetRuntimeState::Get().GetLoadService();
			auto& State = *Handle->Impl;
			DPackage* Package = nullptr;
			DObject* Object = nullptr;
			FAssetReadResult Result;
			{
				// Preserve the synchronous loader's SCC ordering, rollback and publication.
				// No uncaptured dependency may silently perform disk I/O on GameThread.
				const auto* Previous = std::exchange(Loader.AsyncInputs, &Inputs);
				struct FRestore { decltype(Loader.AsyncInputs)& Slot; decltype(Previous) Value;
					~FRestore() { Slot = Value; } } Restore{Loader.AsyncInputs, Previous};
				if (State.ObjectPath)
				{
					auto Loaded = Loader.LoadObject(*State.ObjectPath, State.ExpectedClass, &State.Report);
					Object = Loaded.value_or(nullptr);
					if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
					Package = Object ? Object->GetPackage() : nullptr;
				}
				else
				{
					auto Loaded = Loader.LoadPackage(State.PackagePath, &State.Report);
					Package = Loaded.value_or(nullptr);
					if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
				}
			}
			Complete(Handle, std::move(Result), Package, Object);
		}

	public:
		auto Request(FPackagePath PackagePath, std::optional<FObjectPath> ObjectPath,
			const DClass* ExpectedClass, FAsyncLoadCallback Callback, int32 Priority)
			-> std::shared_ptr<FAsyncLoadHandle>
		{
			CheckAsyncLoadThread();
			auto Handle = std::shared_ptr<FAsyncLoadHandle>(new FAsyncLoadHandle());
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

		auto Process(double Milliseconds, uint32 MaximumCompletions) -> void
		{
			CheckAsyncLoadThread();
			if (bProcessing || MaximumCompletions == 0) return;
			if (!AssetPrivate::FAssetLiveLoadGuard::Check("async load pump", "")) return;
			bProcessing = true;
			struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bProcessing};
			const auto Start = std::chrono::steady_clock::now();
			uint32 Completed = 0;
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
				if (Callback)
				{
					try { Callback(*Handle); }
					catch (...) { DURIN_ERROR("Async asset load completion callback threw an exception."); }
					++Completed;
				}
			}
			for (size_t Index = 0; Index < Jobs.size() && HasBudget();)
			{
				auto& Job = *Jobs[Index];
				if (std::ranges::all_of(Job.Handles, [](const auto& H) { return H->IsComplete(); }))
					Job.Cancellation.RequestCancellation();
				if (!Job.Read.IsCompleted()) { ++Index; continue; }
				auto Finished = std::move(Jobs[Index]);
				Jobs.erase(Jobs.begin() + Index);
				FAssetReadResult Result;
				FAsyncPackageInputs Inputs;
				if (Finished->Read.GetState() != ETaskState::Succeeded)
					Result = {Finished->Read.GetState() == ETaskState::Canceled
						? EAssetReadError::Cancelled : EAssetReadError::IoError, "Async package read did not succeed."};
				else Inputs = std::move(Finished->Read).TakeResult();
				if (Result && Finished->Revision != GetAssetCatalogRevision())
					Result = {EAssetReadError::StaleData, "Asset catalog changed during async loading; retry the request."};
				for (auto& Handle : Finished->Handles)
				{
					if (Result) Apply(Handle, Inputs);
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
					if (!Resolution) { Apply(Handle, {}); ++Completed; continue; }
					Root = Resolution.FinalPath.GetPackagePath();
				}
				if (!Root.IsValid())
				{
					Complete(Handle, {EAssetReadError::InvalidPath, "Async load requires a valid package path."});
					++Completed; continue;
				}
				if (FindResidentPackage(Root)) { Apply(Handle, {}); ++Completed; continue; }
				const auto Existing = std::ranges::find_if(Jobs, [&](const auto& Job) { return Job->Path == Root; });
				if (Existing != Jobs.end() && !(*Existing)->Cancellation.IsCancellationRequested())
				{
					(*Existing)->Handles.push_back(Handle);
					State.State = EAsyncLoadState::Loading;
					continue;
				}
				if (Jobs.size() >= MaximumConcurrentReads) { Pending.push_back(Handle); continue; }
				if (!IsTaskSchedulerRunning())
				{
					Complete(Handle, {EAssetReadError::InUse, "Async loading requires a running task scheduler."});
					++Completed; continue;
				}
				auto Job = std::make_unique<FJob>();
				Job->Path = Root;
				Job->Revision = GetAssetCatalogRevision();
				Job->Handles.push_back(Handle);
				std::vector<FAssetData> Sources;
				std::vector<FPackagePath> Paths{Root};
				std::unordered_set<FPackagePath> Visited;
				while (!Paths.empty())
				{
					const auto Path = Paths.back(); Paths.pop_back();
					if (!Visited.insert(Path).second || FindResidentPackage(Path)) continue;
					const auto Entry = FindAssetExact(Path);
					if (!Entry) continue; // Only the live serializer decides which dependencies are needed.
					Paths.insert(Paths.end(), Entry->Dependencies.begin(), Entry->Dependencies.end());
					if (Entry->RedirectDestination.IsValid()) Paths.push_back(Entry->RedirectDestination);
					Sources.push_back(*Entry);
				}
				if (!Group) Group = std::make_unique<Tasks::FTaskGroup>();
				Job->Read = Tasks::LaunchTask(*Group, Tasks::ETaskExecutor::BlockingIO,
					{.DebugName = "Asset.PackageRead", .Cancellation = Job->Cancellation.GetToken()},
					[Sources = std::move(Sources)](const FTaskCancellationToken& Token) mutable {
						return ReadInputs(std::move(Sources), Token);
					});
				State.State = EAsyncLoadState::Loading;
				Jobs.push_back(std::move(Job));
			}
			if (Jobs.empty() && Group)
			{
				Group->Close();
				Group.reset();
			}
		}

		auto Shutdown() -> void
		{
			CheckAsyncLoadThread();
			if (!AssetPrivate::FAssetLiveLoadGuard::Check("async load cancellation", "")) return;
			for (auto& Handle : Pending) Handle->Cancel();
			Pending.clear();
			for (auto& Handle : Deliveries) Handle->Cancel();
			Deliveries.clear();
			if (Group) Group->Close(ETaskScopeCloseMode::Cancel);
			for (auto& Job : Jobs)
			{
				for (auto& Handle : Job->Handles) Handle->Cancel();
				Job->Cancellation.RequestCancellation();
				(void)WaitTask(Job->Read.GetCompletion().GetTaskHandle());
			}
			Jobs.clear();
			Group.reset();
		}
	};

	namespace { auto AsyncLoader() -> FAsyncAssetLoadService& { static FAsyncAssetLoadService Loader; return Loader; } }
	auto LoadPackageAsync(const FPackagePath& Path, FAsyncLoadCallback Callback, int32 Priority)
		-> std::shared_ptr<FAsyncLoadHandle>
	{
		return AsyncLoader().Request(Path, {}, nullptr, std::move(Callback), Priority);
	}
	auto RequestAsyncLoad(const FObjectPath& Path, FAsyncLoadCallback Callback,
		const DClass* ExpectedClass, int32 Priority) -> std::shared_ptr<FAsyncLoadHandle>
	{
		return AsyncLoader().Request(Path.GetPackagePath(), Path, ExpectedClass, std::move(Callback), Priority);
	}
	auto RequestAsyncLoad(const FTopLevelAssetPath& Path, FAsyncLoadCallback Callback,
		const DClass* ExpectedClass, int32 Priority) -> std::shared_ptr<FAsyncLoadHandle>
	{
		FObjectPath ObjectPath;
		FObjectPath::TryCreate(Path, std::span<const std::string>{}, ObjectPath);
		return RequestAsyncLoad(ObjectPath, std::move(Callback), ExpectedClass, Priority);
	}
	auto ProcessAsyncLoading(double Milliseconds, uint32 MaximumCompletions) -> void { AsyncLoader().Process(Milliseconds, MaximumCompletions); }
	auto CancelAsyncLoading() -> void { AsyncLoader().Shutdown(); }
}
