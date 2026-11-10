#include "Engine/StreamableManager.h"
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

	// Handles keep only a weak owner link, so observations can outlive a manager.
	struct FStreamableManagerState
	{
		FStreamableManager* Owner = nullptr;
		bool bDestroying = false;
		std::vector<std::weak_ptr<FStreamableHandle>> Requests;
		std::vector<std::shared_ptr<FStreamableHandle>> Managed;
	};
	namespace
	{
		auto ManagerStates() -> std::vector<std::weak_ptr<FStreamableManagerState>>&
		{
			static std::vector<std::weak_ptr<FStreamableManagerState>> States;
			return States;
		}
	}

	struct FStreamableHandle::FImpl
	{
		EAssetLoadState State = EAssetLoadState::Pending;
		std::optional<FAssetReadError> Failure;
		FAssetLoadReport Report;
		FPackagePath PackagePath;
		std::optional<FSoftObjectPath> ObjectPath;
		const DClass* ExpectedClass = nullptr;
		int32 Priority = 0;
		FStreamableDelegate Callback;
		FStreamableDelegate CancelCallback;
		bool bLoadCompleted = false;
		bool bCanceled = false;
		bool bAggregate = false;
		bool bOwnChildren = false;
		std::weak_ptr<FStreamableManagerState> Manager;
		std::string DebugName;
		std::vector<FSoftObjectPath> Targets;
		std::vector<FAssetReadError> TargetErrors;
		std::vector<FAssetLoadReport> Reports;
		std::vector<std::shared_ptr<FStreamableHandle>> Children;
		std::vector<std::weak_ptr<FStreamableHandle>> Parents;
		std::vector<TStrongObjectPtr<DObject>> Assets;
		std::vector<TStrongObjectPtr<DPackage>> AssetPackages;
		std::vector<TStrongObjectPtr<DPackage>> ChildPackageRoots;
		std::vector<TWeakObjectPtr<DPackage>> WeakPackageRoots;
		std::vector<TWeakObjectPtr<DObject>> WeakAssets;
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
		return !Impl->bReleased && !Impl->bCanceled && (Impl->State == EAssetLoadState::Pending
			|| Impl->State == EAssetLoadState::Loading || Impl->State == EAssetLoadState::Succeeded
			|| Impl->Package.IsValid()
			|| std::ranges::any_of(Impl->Assets, [](const auto& Asset) { return Asset.IsValid(); }));
	}

	auto FStreamableHandle::HasLoadCompleted() const -> bool { CheckAsyncLoadThread(); return Impl->bLoadCompleted; }
	auto FStreamableHandle::WasCanceled() const -> bool { CheckAsyncLoadThread(); return Impl->bCanceled; }
	auto FStreamableHandle::HasError() const -> bool { CheckAsyncLoadThread(); return Impl->Failure.has_value(); }
	auto FStreamableHandle::GetError() const -> const FAssetReadError&
	{
		CheckAsyncLoadThread();
		static const FAssetReadError None{EAssetReadError::None, {}};
		return Impl->Failure ? *Impl->Failure : None;
	}
	auto FStreamableHandle::GetRequestedAssets() const -> const std::vector<FSoftObjectPath>& { CheckAsyncLoadThread(); return Impl->Targets; }
	auto FStreamableHandle::GetTargetErrors() const -> std::vector<FAssetReadError>
	{
		CheckAsyncLoadThread();
		return Impl->bAggregate ? Impl->TargetErrors : std::vector<FAssetReadError>(Impl->Targets.size(), GetError());
	}
	auto FStreamableHandle::GetLoadedAssets() const -> std::vector<DObject*>
	{
		CheckAsyncLoadThread();
		if (!Impl->bAggregate) return Impl->Targets.empty() ? std::vector<DObject*>{} : std::vector<DObject*>{GetLoadedAsset()};
		std::vector<DObject*> Result;
		for (size_t Index = 0; Index < Impl->Targets.size(); ++Index)
			Result.push_back(Impl->bReleased ? Impl->WeakAssets[Index].Get() : Impl->Assets[Index].Get());
		return Result;
	}
	auto FStreamableHandle::GetLoadReports() const -> std::vector<FAssetLoadReport>
	{
		CheckAsyncLoadThread();
		return Impl->bAggregate ? Impl->Reports : std::vector<FAssetLoadReport>(Impl->Targets.size(), Impl->Report);
	}
	auto FStreamableHandle::GetLoadedAsset() const -> DObject*
	{
		CheckAsyncLoadThread();
		if (Impl->Targets.empty()) return nullptr;
		if (!Impl->bAggregate) return Impl->bReleased ? Impl->LoadedObject.Get() : Impl->Object.Get();
		return Impl->bReleased ? Impl->WeakAssets.front().Get() : Impl->Assets.front().Get();
	}
	auto FStreamableHandle::GetDebugName() const -> const std::string& { CheckAsyncLoadThread(); return Impl->DebugName; }
	auto FStreamableHandle::GetOwningManager() const -> FStreamableManager*
	{
		CheckAsyncLoadThread();
		const auto Owner = Impl->Manager.lock();
		return Owner ? Owner->Owner : nullptr;
	}
	auto FStreamableHandle::IsManaged() const -> bool
	{
		CheckAsyncLoadThread();
		const auto Owner = Impl->Manager.lock();
		return Owner && std::ranges::any_of(Owner->Managed, [this](const auto& Handle) { return Handle.get() == this; });
	}
	auto FStreamableHandle::GetReport() const -> const FAssetLoadReport& { CheckAsyncLoadThread(); return Impl->Report; }
	auto FStreamableHandle::GetLoadedPackage() const -> DPackage*
	{
		CheckAsyncLoadThread();
		return Impl->bReleased ? Impl->LoadedPackage.Get() : Impl->Package.Get();
	}
	auto FStreamableHandle::ReleaseRetention() -> void
	{
		if (Impl->bReleased) return;
		const auto KeepAlive = weak_from_this().lock();
		const bool bHadRetention = Impl->Package.IsValid() || Impl->Object.IsValid() || !Impl->Assets.empty();
		Impl->LoadedPackage = Impl->Package.Get();
		Impl->LoadedObject = Impl->Object.Get();
		Impl->Object.Reset();
		Impl->Package.Reset();
		for (size_t Index = 0; Index < Impl->Assets.size(); ++Index)
			Impl->WeakAssets[Index] = Impl->Assets[Index].Get();
		Impl->Assets.clear();
		Impl->AssetPackages.clear();
		Impl->WeakPackageRoots.clear();
		for (const auto& Package : Impl->ChildPackageRoots) Impl->WeakPackageRoots.emplace_back(Package.Get());
		Impl->ChildPackageRoots.clear();
		if (Impl->bOwnChildren)
			for (auto& Child : Impl->Children) if (!Child->IsComplete()) Child->CancelHandle();
		Impl->Children.clear();
		Impl->bReleased = true;
		if (const auto Owner = Impl->Manager.lock())
			std::erase_if(Owner->Managed, [this](const auto& Handle) { return Handle.get() == this; });
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
		if (Impl->bCanceled) return;
		const auto KeepAlive = weak_from_this().lock();
		Impl->bCanceled = true;
		Impl->Callback = {};
		Impl->bCompletionPending = false;
		Impl->bReleaseRequested = true;
		Impl->State = EAssetLoadState::Cancelled;
		Impl->Failure = FAssetReadError{EAssetReadError::Cancelled, "Asset load request cancelled."};
		if (Impl->bAggregate) std::ranges::fill(Impl->TargetErrors, *Impl->Failure);
		auto Callback = std::move(Impl->CancelCallback);
		ReleaseRetention();
		if (Callback)
		{
			try { Callback(*this); }
			catch (...) { DURIN_ERROR("Async asset cancellation callback threw an exception."); }
		}
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
		std::vector<std::shared_ptr<FStreamableHandle>> Aggregates;
		bool bProcessing = false;
		bool bStopping = false;

		auto Complete(const std::shared_ptr<FStreamableHandle>& Handle, FAssetReadResult Result,
			DPackage* Package = nullptr, DObject* Object = nullptr) -> void
		{
			if (Handle->IsComplete()) return;
			auto& State = *Handle->Impl;
			State.bLoadCompleted = true;
			State.State = Result ? EAssetLoadState::Succeeded : EAssetLoadState::Failed;
			if (Result)
			{
				if (!State.bAggregate) { State.Package = Package; State.Object = Object; }
				State.LoadedPackage = State.Package.Get(); State.LoadedObject = State.Object.Get();
			}
			else State.Failure = AssetReadErrorFromResult(Result);
			State.bCompletionPending = static_cast<bool>(State.Callback);
			// Capture parents before a child's deferred release can drop its GC roots.
			for (const auto& WeakParent : State.Parents)
				if (auto Parent = WeakParent.lock()) RefreshAggregate(Parent);
			if (State.Callback) Deliveries.push_back(Handle);
			else if (State.bReleaseRequested) Handle->ReleaseRetention();
			if (!Handle->IsActive())
				if (const auto Owner = State.Manager.lock())
					std::erase_if(Owner->Managed, [&](const auto& Entry) { return Entry == Handle; });
		}

		auto RefreshAggregate(const std::shared_ptr<FStreamableHandle>& Handle) -> void
		{
			if (Handle->IsComplete()) return;
			auto& State = *Handle->Impl;
			bool bComplete = true;
			FAssetReadResult Result;
			size_t Slot = 0;
			std::vector<TStrongObjectPtr<DPackage>> Packages;
			auto RetainPackage = [&](DPackage* Package) {
				if (Package && std::ranges::none_of(Packages, [&](const auto& Root) { return Root.Get() == Package; }))
					Packages.emplace_back(Package);
			};
			for (const auto& Child : State.Children)
			{
				// Child completion side effects must publish before the parent becomes terminal.
				if (!Child->IsComplete() || Child->Impl->bCompletionPending) bComplete = false;
				if (Result && Child->HasError()) Result = AssetReadResultFromError(Child->GetError());
				DPackage* Package = Child->WasCanceled() ? nullptr : Child->GetLoadedPackage();
				RetainPackage(Package);
				if (!Child->WasCanceled() && Child->Impl->bAggregate)
				{
					if (Child->Impl->bReleased)
						for (const auto& Root : Child->Impl->WeakPackageRoots) RetainPackage(Root.Get());
					else
						for (const auto& Root : Child->Impl->ChildPackageRoots) RetainPackage(Root.Get());
				}
				auto Assets = Child->GetLoadedAssets();
				if (Child->WasCanceled()) std::ranges::fill(Assets, nullptr);
				const auto Errors = Child->GetTargetErrors();
				const auto Reports = Child->GetLoadReports();
				for (size_t Index = 0; Index < Assets.size(); ++Index, ++Slot)
				{
					State.Assets[Slot] = Assets[Index];
					State.WeakAssets[Slot] = Assets[Index];
					State.AssetPackages[Slot] = Assets[Index] ? Assets[Index]->GetPackage() : nullptr;
					State.TargetErrors[Slot] = Errors[Index];
					State.Reports[Slot] = Reports[Index];
				}
			}
			State.Package = Packages.empty() ? nullptr : Packages.front().Get();
			State.ChildPackageRoots = std::move(Packages);
			if (!State.Reports.empty())
			{
				const auto FirstError = std::ranges::find_if(State.TargetErrors,
					[](const auto& Error) { return Error.Code != EAssetReadError::None; });
				const size_t Index = FirstError == State.TargetErrors.end() ? 0 : FirstError - State.TargetErrors.begin();
				State.Report = State.Reports[Index];
			}
			if (bComplete) Complete(Handle, Result);
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
			{
				// Preserve the synchronous loader's SCC ordering, rollback and publication.
				// No uncaptured dependency may silently perform disk I/O on GameThread.
				const auto* Previous = std::exchange(Loader.AsyncInputs, Inputs);
				struct FRestore { decltype(Loader.AsyncInputs)& Slot; decltype(Previous) Value;
					~FRestore() { Slot = Value; } } Restore{Loader.AsyncInputs, Previous};
				if (State.ObjectPath)
				{
					auto Loaded = LoadObjectDetailed(*State.ObjectPath, State.ExpectedClass, &State.Report);
					Object = Loaded.value_or(nullptr);
					if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
					Package = Object ? Object->GetPackage() : nullptr;
				}
				else
				{
					auto Loaded = LoadPackageDetailed(State.PackagePath, &State.Report);
					Package = Loaded.value_or(nullptr);
					if (!Loaded) Result = AssetReadResultFromError(Loaded.error());
				}
			}
			Complete(Handle, std::move(Result), Package, Object);
		}

	public:
		auto Request(FPackagePath PackagePath, std::optional<FSoftObjectPath> ObjectPath,
			const DClass* ExpectedClass, FStreamableDelegate Callback, int32 Priority)
			-> std::shared_ptr<FStreamableHandle>
		{
			CheckAsyncLoadThread();
			auto Handle = std::shared_ptr<FStreamableHandle>(new FStreamableHandle());
			auto& State = *Handle->Impl;
			State.PackagePath = std::move(PackagePath);
			State.ObjectPath = std::move(ObjectPath);
			if (State.ObjectPath) State.Targets.push_back(*State.ObjectPath);
			State.ExpectedClass = ExpectedClass;
			State.Callback = std::move(Callback);
			State.Priority = Priority;
			State.Report.RequestedPath = State.PackagePath;
			State.Report.RequestedObjectPath = State.ObjectPath.value_or(FSoftObjectPath{});
			// Rejected lifetime/guard admissions must never enqueue work for later escape.
			auto Admission = AssetPrivate::FAssetLiveLoadGuard::Check("async load", State.PackagePath.ToString());
			if (Admission && bStopping) Admission = {EAssetReadError::ShuttingDown, "Streamable requests are draining."};
			if (Admission) Admission = FAssetRuntimeState::Get().CheckRequestAdmission();
			if (!Admission)
			{
				State.Callback = {};
				Complete(Handle, std::move(Admission));
			}
			else Pending.push_back(Handle);
			return Handle;
		}

		auto RequestSync(FPackagePath PackagePath, std::optional<FSoftObjectPath> ObjectPath,
			const DClass* ExpectedClass) -> std::shared_ptr<FStreamableHandle>
		{
			CheckAsyncLoadThread();
			auto Handle = std::shared_ptr<FStreamableHandle>(new FStreamableHandle());
			Handle->Impl->PackagePath = std::move(PackagePath);
			Handle->Impl->ObjectPath = std::move(ObjectPath);
			if (Handle->Impl->ObjectPath) Handle->Impl->Targets.push_back(*Handle->Impl->ObjectPath);
			Handle->Impl->ExpectedClass = ExpectedClass;
			Handle->Impl->Report.RequestedPath = Handle->Impl->PackagePath;
			Handle->Impl->Report.RequestedObjectPath = Handle->Impl->ObjectPath.value_or(FSoftObjectPath{});
			auto Admission = AssetPrivate::FAssetLiveLoadGuard::Check("sync load", Handle->Impl->PackagePath.ToString());
			if (Admission && bStopping) Admission = {EAssetReadError::ShuttingDown, "Streamable requests are draining."};
			if (Admission) Admission = FAssetRuntimeState::Get().CheckRequestAdmission();
			if (Admission && (!Handle->Impl->PackagePath.IsValid()
				|| (Handle->Impl->ObjectPath && !Handle->Impl->ObjectPath->IsValid())))
				Admission = {EAssetReadError::InvalidPath, "A load request requires a valid package or object path."};
			if (Admission) Apply(Handle);
			else Complete(Handle, std::move(Admission));
			return Handle;
		}

		auto Combine(std::span<const std::shared_ptr<FStreamableHandle>> Children,
			FStreamableDelegate Callback, bool bOwnChildren, bool bDeferred) -> std::shared_ptr<FStreamableHandle>
		{
			CheckAsyncLoadThread();
			auto Handle = std::shared_ptr<FStreamableHandle>(new FStreamableHandle());
			auto& State = *Handle->Impl;
			State.bAggregate = true;
			State.bOwnChildren = bOwnChildren;
			State.Callback = std::move(Callback);
			auto Admission = AssetPrivate::FAssetLiveLoadGuard::Check("combined load", "");
			if (Admission && bStopping) Admission = {EAssetReadError::ShuttingDown, "Streamable requests are draining."};
			if (Admission) Admission = FAssetRuntimeState::Get().CheckRequestAdmission();
			if (!Admission)
			{
				State.Callback = {};
				Complete(Handle, Admission);
				return Handle;
			}
			for (const auto& Child : Children)
			{
				if (!Child)
				{
					State.Children.push_back(RequestSync({}, {}, nullptr));
					continue;
				}
				State.Children.push_back(Child);
				const auto& Targets = Child->GetRequestedAssets();
				State.Targets.insert(State.Targets.end(), Targets.begin(), Targets.end());
				Child->Impl->Parents.push_back(Handle);
			}
			State.Assets.resize(State.Targets.size());
			State.WeakAssets.resize(State.Targets.size());
			State.AssetPackages.resize(State.Targets.size());
			State.TargetErrors.resize(State.Targets.size(), {EAssetReadError::None, {}});
			State.Reports.resize(State.Targets.size());
			for (size_t Index = 0; Index < State.Targets.size(); ++Index)
			{
				State.Reports[Index].RequestedObjectPath = State.Targets[Index];
				State.Reports[Index].RequestedPath = State.Targets[Index].GetPackagePath();
			}
			if (bDeferred) Aggregates.push_back(Handle);
			else RefreshAggregate(Handle);
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
			const auto Parents = Aggregates;
			for (const auto& Parent : Parents) RefreshAggregate(Parent);
			std::erase_if(Aggregates, [](const auto& Handle) { return Handle->IsComplete(); });

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
			for (const auto& Parent : Aggregates) RefreshAggregate(Parent);
			std::erase_if(Aggregates, [](const auto& Handle) { return Handle->IsComplete(); });

		}

		auto Shutdown() -> void
		{
			CheckAsyncLoadThread();
			if (!AssetPrivate::FAssetLiveLoadGuard::Check("async load cancellation", "")) return;
			if (bStopping) return;
			bStopping = true;
			struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bStopping};
			// Cancellation delegates can create managers and submit/reenter drains.
			// Snapshot owner/queue containers and reject new requests until this drain ends.
			const auto Owners = ManagerStates();
			auto Parents = std::exchange(Aggregates, {});
			auto Requests = std::exchange(Pending, {});
			auto Callbacks = std::exchange(Deliveries, {});
			auto Reads = std::exchange(Jobs, {});
			for (const auto& WeakOwner : Owners)
				if (const auto Owner = WeakOwner.lock())
				{
					const auto Handles = Owner->Requests;
					for (const auto& Weak : Handles) if (const auto Handle = Weak.lock()) Handle->CancelHandle();
					Owner->Managed.clear();
				}
			for (auto& Handle : Parents) Handle->CancelHandle();
			for (auto& Handle : Requests) Handle->CancelHandle();
			for (auto& Handle : Callbacks) Handle->CancelHandle();
			for (auto& Job : Reads)
			{
				for (auto& Handle : Job->Handles) Handle->CancelHandle();
				Job->Cancellation.Cancel();
				(void)WaitTask(Durin::Tasks::FTask(Job->Read));
			}
		}
	};

	namespace { auto AsyncLoader() -> FAssetLoadRequestService& { static FAssetLoadRequestService Loader; return Loader; } }
	FStreamableManager::FStreamableManager() : State(std::make_shared<FStreamableManagerState>())
	{
		CheckAsyncLoadThread();
		State->Owner = this;
		std::erase_if(ManagerStates(), [](const auto& Owner) { return Owner.expired(); });
		ManagerStates().push_back(State);
	}
	FStreamableManager::~FStreamableManager()
	{
		CheckAsyncLoadThread();
		State->bDestroying = true;
		State->Owner = nullptr;
		const auto Requests = State->Requests;
		for (const auto& Weak : Requests)
			if (const auto Handle = Weak.lock())
			{
				Handle->Impl->Callback = {};
				Handle->Impl->CancelCallback = {};
				Handle->CancelHandle();
			}
		State->Managed.clear();
	}
	auto FStreamableManager::Track(std::shared_ptr<FStreamableHandle> Handle, bool bManaged) -> std::shared_ptr<FStreamableHandle>
	{
		Handle->Impl->Manager = State;
		std::erase_if(State->Requests, [](const auto& Weak) { return Weak.expired(); });
		State->Requests.push_back(Handle);
		if (State->bDestroying) { Handle->Impl->Callback = {}; Handle->Impl->CancelCallback = {}; Handle->CancelHandle(); }
		else if (bManaged && Handle->IsActive()) State->Managed.push_back(Handle);
		return Handle;
	}
	auto FStreamableManager::RequestAsyncLoad(FStreamableAsyncLoadParams Params) -> std::shared_ptr<FStreamableHandle>
	{
		std::vector<std::shared_ptr<FStreamableHandle>> Children;
		for (const auto& Path : Params.TargetsToStream)
			Children.push_back(AsyncLoader().Request(Path.GetPackagePath(), Path, Params.ExpectedClass, {}, Params.Priority));
		auto Handle = AsyncLoader().Combine(Children, std::move(Params.OnComplete), true, true);
		if (!Handle->IsComplete()) Handle->Impl->CancelCallback = std::move(Params.OnCancel);
		Handle->Impl->DebugName = std::move(Params.DebugName);
		return Track(std::move(Handle), Params.bManageActiveHandle);
	}
	auto FStreamableManager::RequestAsyncLoad(const FSoftObjectPath& Path, FStreamableDelegate Callback,
		const DClass* ExpectedClass, int32 Priority) -> std::shared_ptr<FStreamableHandle>
	{
		return RequestAsyncLoad({.TargetsToStream = {Path}, .OnComplete = std::move(Callback), .Priority = Priority, .ExpectedClass = ExpectedClass});
	}
	auto FStreamableManager::RequestAsyncLoad(const FTopLevelAssetPath& Path, FStreamableDelegate Callback,
		const DClass* ExpectedClass, int32 Priority) -> std::shared_ptr<FStreamableHandle>
	{
		FSoftObjectPath ObjectPath;
		FSoftObjectPath::TryCreate(Path, std::span<const std::string>{}, ObjectPath);
		return RequestAsyncLoad(ObjectPath, std::move(Callback), ExpectedClass, Priority);
	}
	auto FStreamableManager::RequestSyncLoad(std::span<const FSoftObjectPath> Targets, bool bManaged) -> std::shared_ptr<FStreamableHandle>
	{
		std::vector<std::shared_ptr<FStreamableHandle>> Children;
		for (const auto& Path : Targets) Children.push_back(AsyncLoader().RequestSync(Path.GetPackagePath(), Path, nullptr));
		return Track(AsyncLoader().Combine(Children, {}, true, false), bManaged);
	}
	auto FStreamableManager::RequestSyncLoad(const FSoftObjectPath& Path, const DClass* ExpectedClass) -> std::shared_ptr<FStreamableHandle>
	{
		return Track(AsyncLoader().RequestSync(Path.GetPackagePath(), Path, ExpectedClass));
	}
	auto FStreamableManager::RequestSyncLoad(const FTopLevelAssetPath& Path, const DClass* ExpectedClass) -> std::shared_ptr<FStreamableHandle>
	{
		FSoftObjectPath ObjectPath;
		FSoftObjectPath::TryCreate(Path, std::span<const std::string>{}, ObjectPath);
		return RequestSyncLoad(ObjectPath, ExpectedClass);
	}
	auto FStreamableManager::RequestAsyncLoadPackage(const FPackagePath& Path, FStreamableDelegate Callback, int32 Priority) -> std::shared_ptr<FStreamableHandle>
	{
		return Track(AsyncLoader().Request(Path, {}, nullptr, std::move(Callback), Priority));
	}
	auto FStreamableManager::RequestSyncLoadPackage(const FPackagePath& Path) -> std::shared_ptr<FStreamableHandle>
	{
		return Track(AsyncLoader().RequestSync(Path, {}, nullptr));
	}
	auto FStreamableManager::CreateCombinedHandle(std::span<const std::shared_ptr<FStreamableHandle>> Children,
		FStreamableDelegate Callback, std::string DebugName, bool bManaged, FStreamableDelegate OnCancel) -> std::shared_ptr<FStreamableHandle>
	{
		auto Handle = AsyncLoader().Combine(Children, std::move(Callback), false, true);
		if (!Handle->IsComplete()) Handle->Impl->CancelCallback = std::move(OnCancel);
		Handle->Impl->DebugName = std::move(DebugName);
		return Track(std::move(Handle), bManaged);
	}
	auto FStreamableManager::GetActiveHandles() const -> std::vector<std::shared_ptr<FStreamableHandle>>
	{
		CheckAsyncLoadThread();
		std::vector<std::shared_ptr<FStreamableHandle>> Result;
		for (const auto& Weak : State->Requests) if (auto Handle = Weak.lock(); Handle && Handle->IsActive()) Result.push_back(std::move(Handle));
		return Result;
	}
	auto GetStreamableManager() -> FStreamableManager& { static FStreamableManager Manager; return Manager; }

	auto ProcessAsyncLoading(double Milliseconds, uint32 MaximumCompletions) -> void { AsyncLoader().Process(Milliseconds, MaximumCompletions); }
	auto CancelAsyncLoading() -> void { AsyncLoader().Shutdown(); }
}
