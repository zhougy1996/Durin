#include "DObject/PackagePersistence.h"
#include "CoreGlobals.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/FilePublication.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace PackageSavePrivate
	{
		struct FAsyncSaveState
		{
			uint64 Bytes = 0;
			Tasks::FTask Disk, Publication;
			bool bAdmitted = false;
			std::function<void()> Cleanup;
		};
	}
	namespace
	{
		FPackageDestinationResolver DestinationResolver;
		std::unordered_set<const DPackage*> PreparingPackages;
		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
		auto Fail(UnifiedError::FErrorId Error, std::string Message) -> FPackageWriteResult
		{ return {.Error = UnifiedError::FError{Error, std::move(Message)}}; }
		std::mutex AsyncSaveMutex;
		std::vector<std::shared_ptr<PackageSavePrivate::FAsyncSaveState>> AsyncSaves;
		bool bAcceptAsyncSaves = true;
		uint32 SavePublicationDepth = 0;
		bool bDrainingSaves = false;
		PackageSavePrivate::EPublicationFault PublicationFault = PackageSavePrivate::EPublicationFault::None;
		uint32 AsyncSaveMaxOperations = 64;
		uint64 AsyncSaveMaxBytes = 256ull * 1024 * 1024;
		auto SnapshotSaves() -> std::vector<std::shared_ptr<PackageSavePrivate::FAsyncSaveState>>
		{ std::lock_guard Lock(AsyncSaveMutex); return AsyncSaves; }
		auto RetireAsyncSave(const std::shared_ptr<PackageSavePrivate::FAsyncSaveState>& Save) -> void
		{
			check(IsInGameThread());
			check(Save->Disk.IsCompleted() && Save->Publication.IsCompleted());
			++SavePublicationDepth;
			auto Cleanup = std::move(Save->Cleanup);
			try { if (Cleanup) Cleanup(); }
			catch (...) { DURIN_ERROR("Async package save cleanup threw an exception."); }
			Cleanup = {};
			--SavePublicationDepth;
			std::lock_guard Lock(AsyncSaveMutex);
			std::erase(AsyncSaves, Save);
		}
	}
	namespace PackageSavePrivate
	{
		auto CheckAsyncAdmission(uint64 Bytes) -> std::expected<void, UnifiedError::FError>
		{
			check(IsInGameThread());
			if (!bAcceptAsyncSaves || !IsTaskSchedulerRunning()
				|| !GetGameThreadDeferredWorkQueueDiagnostics().bAccepting)
				return std::unexpected(UnifiedError::FError{PackageSaveErrors::ShuttingDown, "Async saves require an accepting GameThread executor."});
			if (SavePublicationDepth != 0 || bDrainingSaves) return std::unexpected(UnifiedError::FError{PackageSaveErrors::Busy, "Async save submission during publication or drain is unsupported."});
			uint64 Retained = 0;
			std::lock_guard Lock(AsyncSaveMutex);
			for (const auto& Save : AsyncSaves) Retained += Save->Bytes;
			if (AsyncSaves.size() >= AsyncSaveMaxOperations || Bytes > AsyncSaveMaxBytes - Retained)
				return std::unexpected(UnifiedError::FError{PackageSaveErrors::Busy, "Async save capacity is exhausted."});
			return {};
		}
		auto BeginAsyncSavePublication(const std::shared_ptr<FAsyncSaveState>& Save) -> void
		{
			check(IsInGameThread());
			check(Save->Disk.IsCompleted());
			++SavePublicationDepth;
		}
		auto EndAsyncSavePublication() -> void { check(IsInGameThread() && SavePublicationDepth != 0); --SavePublicationDepth; }
		auto ReportAsyncSaveAbandonmentException() -> void
		{ DURIN_ERROR("Async save abandonment threw an exception."); }

		auto SubmitAsyncSaveTask(uint64 Bytes, std::function<void()> Write,
			FAsyncSavePublicationFactory MakePublication, std::function<void()> Cleanup) -> std::expected<void, UnifiedError::FError>
		{
			if (auto Result = CheckAsyncAdmission(Bytes); !Result) return Result;
			// Reserve ownership before creating accepted work; registration below cannot allocate.
			{ std::lock_guard Lock(AsyncSaveMutex); AsyncSaves.reserve(AsyncSaves.size() + 1); }
			auto Save = std::make_shared<FAsyncSaveState>();
			Save->Bytes = Bytes; Save->Cleanup = std::move(Cleanup);
			// Only submission rollback and the real I/O task share this slot. The
			// owner retains object cleanup separately, never the write closure.
			auto WriteBody = std::make_shared<std::function<void()>>(std::move(Write));
			Tasks::FTaskEvent Gate("Package.SaveAdmission");
			try
			{
				Save->Disk = Tasks::LaunchTask("Package.SaveIO", [Save, WriteBody] {
						if (!Save->bAdmitted) {
							Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
							return;
						}
						auto Invoke = std::exchange(*WriteBody, {});
						Invoke();
					}, Gate, Tasks::ETaskThread::BlockingIO);
				if (PublicationFault == EPublicationFault::RejectAdmission)
				{
					*WriteBody = {}; Save->Cleanup = {};
					Gate.Trigger();
					return std::unexpected(UnifiedError::FError{PackageSaveErrors::ShuttingDown, "Injected save publication admission failure."});
				}
				Save->Publication = MakePublication(Save->Disk, Save);
				{ std::lock_guard Lock(AsyncSaveMutex); AsyncSaves.push_back(Save); }
				Save->bAdmitted = true;
				Gate.Trigger();
				return {};
			}
			catch (...)
			{
				*WriteBody = {}; Save->Cleanup = {};
				Gate.Trigger();
				throw;
			}
		}
		auto PollAsyncSaves() -> void
		{
			check(IsInGameThread());
			if (SavePublicationDepth != 0) return;
			for (const auto& Save : SnapshotSaves())
				if (Save->Disk.IsCompleted() && Save->Publication.IsCompleted()) RetireAsyncSave(Save);
		}
		auto SetAsyncSaveAdmission(bool bAccepting) -> void
		{ if (GIsGameThreadIdInitialized) check(IsInGameThread()); bAcceptAsyncSaves = bAccepting; }
		auto SetAsyncSavePublicationFaultForTests(EPublicationFault Fault) -> void
		{ check(IsInGameThread()); PublicationFault = Fault; }
		auto SetAsyncSaveLimitsForTests(uint32 MaxOperations, uint64 MaxBytes) -> void
		{
			check(IsInGameThread());
			std::lock_guard Lock(AsyncSaveMutex);
			check(AsyncSaves.empty());
			AsyncSaveMaxOperations = MaxOperations; AsyncSaveMaxBytes = MaxBytes;
		}
	}
	auto DPackage::HasAsyncFileWrites() -> bool
	{
		for (const auto& Save : SnapshotSaves()) if (!Save->Disk.IsCompleted()) return true;
		return false;
	}
	auto DPackage::WaitForAsyncFileWrites() -> FTaskWaitResult
	{
		ETaskState State = ETaskState::Succeeded;
		for (const auto& Save : SnapshotSaves())
		{
			auto Result = Save->Disk.WaitDetailed();
			if (Result.WaitStatus != ETaskWaitStatus::Completed) return Result;
			if (Result.TaskState == ETaskState::Failed) State = ETaskState::Failed;
			else if (Result.TaskState == ETaskState::Canceled && State != ETaskState::Failed) State = ETaskState::Canceled;
		}
		return {ETaskWaitStatus::Completed, State};
	}
	auto DPackage::DrainAsyncSaves(std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		// Configuration-only tools may shut down before creating a GameThread.
		if (!GIsGameThreadIdInitialized && SnapshotSaves().empty()) return true;
		if (!GIsGameThreadIdInitialized || !IsInGameThread() || SavePublicationDepth != 0 || bDrainingSaves)
			return Reject(OutError, UnifiedError::FError{PackageSaveErrors::Busy, "Save publication drain requires a non-reentrant GameThread caller."});
		bDrainingSaves = true;
		struct FScope { ~FScope() { bDrainingSaves = false; } } Scope;
		const auto Cutoff = SnapshotSaves();
		for (const auto& Save : Cutoff)
			if (!Save->Disk.Wait())
				return Reject(OutError, UnifiedError::FError{PackageSaveErrors::Busy, "Save I/O wait is unsupported on this thread."});
		for (const auto& Save : Cutoff)
		{
			while (!Save->Publication.IsCompleted())
			{
				const auto Before = GetGameThreadDeferredWorkQueueDiagnostics();
				if (!Before.bInstalled) return Reject(OutError, UnifiedError::FError{PackageSaveErrors::Busy, "Save publication executor is unavailable."});
				const auto Pumped = PumpGameThreadDeferredWork({.MaxCallbacks = 1});
				if (!Pumped.ExecutedCallbacks && !Pumped.TerminalEntriesSkipped
					&& GetGameThreadDeferredWorkQueueDiagnostics().ReentrantPumpCount != Before.ReentrantPumpCount)
					return Reject(OutError, UnifiedError::FError{PackageSaveErrors::Busy, "Cannot drain saves from a deferred callback."});
				if (!Pumped.ExecutedCallbacks && !Pumped.TerminalEntriesSkipped) std::this_thread::yield();
			}
		}
		PackageSavePrivate::PollAsyncSaves();
		return true;
	}
	auto SetPackageDestinationResolver(FPackageDestinationResolver Resolver) -> void
	{
		check(IsInGameThread());
		DestinationResolver = std::move(Resolver);
	}
	struct FPackageSaveOperation::FState
	{
		TStrongObjectPtr<DPackage> Package;
		std::vector<FStrongObjectPtr> Exports;
		std::vector<FObjectKey> DiskExports;
		FXxHash128 MainDigest{};
		FPackagePath Identity;
		uint64 EditRevision = 0;
		uint64 DetachedBytes = 0;
		FSavePackageContext Context;
		std::unique_ptr<IPackageWriteOperation> Write;
		Tasks::FTask Worker;
		std::optional<UnifiedError::FError> StagingError;
		std::optional<FPackageWriteResult> Result;
		bool bCommitted = false;
		bool bCommitting = false;
		bool bClearDirty = true;
		auto Stage() -> void
		{
			if (!Write->Stage(&StagingError) && !StagingError)
				StagingError.emplace(PackageSaveErrors::IoError, "Writer rejected staging.");
		}
		auto Cleanup() -> void { Write.reset(); }
	};
	FPackageSaveOperation::FPackageSaveOperation() : State(std::make_unique<FState>()) {}
	FPackageSaveOperation::~FPackageSaveOperation()
	{
		check(IsInGameThread());
		if (State->Worker.IsValid()) State->Worker.Wait();
		if (State->bCommitted) (void)RollbackCommit();
		State->Cleanup();
	}
	auto FPackageSaveOperation::Begin(DPackage* Package, const FPackageSaveOptions& Options,
		std::optional<UnifiedError::FError>* OutError, bool bAsync) -> std::unique_ptr<FPackageSaveOperation>
	{
		return Begin(Package, FSavePackageContext{Options}, OutError, bAsync);
	}
	auto FPackageSaveOperation::Begin(DPackage* Package, const FSavePackageContext& Context,
		std::optional<UnifiedError::FError>* OutError, bool bAsync, bool bDeferStaging) -> std::unique_ptr<FPackageSaveOperation>
	{
		const auto& Options = Context.Options;
		check(IsInGameThread());
		if (OutError) OutError->reset();
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
		{ if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::InvalidPackageType, "A live persistent package is required."}; return {}; }
		if (bAsync && !IsTaskSchedulerRunning())
		{ if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::ShuttingDown, "The task scheduler is not running."}; return {}; }
		if (!PreparingPackages.insert(Package).second)
		{ if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::Busy, "Package preparation is already running."}; return {}; }
		struct FPreparationGuard
		{
			const DPackage* Package;
			~FPreparationGuard() { PreparingPackages.erase(Package); }
		} Guard{Package};
		auto Operation = std::unique_ptr<FPackageSaveOperation>(new FPackageSaveOperation());
		auto& Data = *Operation->State;
		Data.Package = Package;
		for (DObject* Asset : Package->GetTopLevelAssets()) Data.Exports.emplace_back(Asset);
		Data.Context = Context;
		FFileReplacement MainFile, BulkFile;
		FFilePublicationStamp MainStamp, BulkStamp;
		FByteBuffer Bytes, Bulk;
		Data.Identity = Package->GetPackagePathIdentity();
		Data.bClearDirty = !Options.Capture.bCooking && !Options.Capture.PropertyFilter
			&& (!Options.Capture.SaveOverrides || Options.Capture.SaveOverrides->IsEmpty());
		auto Resolver = DestinationResolver;
		MainFile.Destination = Options.Destination.empty() && Resolver ? Resolver(*Package) : Options.Destination;
		if (MainFile.Destination.empty() || MainFile.Destination.extension() != ".dasset")
		{ if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::InvalidPath, "An explicit .dasset destination or configured resolver is required."}; return {}; }
		std::error_code Ec;
		MainFile.Destination = std::filesystem::absolute(MainFile.Destination, Ec).lexically_normal();
		if (Ec) { if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::InvalidPath, Ec.message()}; return {}; }
		auto ReadAccess = FPackageFileAccess::TryReadPackage(MainFile.Destination);
		if (!ReadAccess) { if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::Busy, "Package output is being written."}; return {}; }
		BulkFile.Destination = MainFile.Destination;
		BulkFile.Destination.replace_extension(".dbulk");
		std::optional<FFileSystemError> InspectionError;
		if (!FFilePublicationStamp::Inspect(MainFile.Destination, MainStamp, &InspectionError)
			|| !FFilePublicationStamp::Inspect(BulkFile.Destination, BulkStamp, &InspectionError))
		{ if (OutError) *OutError = ToError(*InspectionError); return {}; }
		ObjectPackage::FLinkerTables Linker;
		Data.EditRevision = Package->GetEditRevision();
		const auto CaptureResult = Data.Context.Capture(Package, Linker, ObjectPackage::DastV12FormatVersion, &Data.DiskExports);
		if (!CaptureResult)
		{
			if (OutError) *OutError = ToError(CaptureResult.error());
			return {};
		}
		ObjectPackage::FPackageWriterResult Diagnostic;
		if (!(Diagnostic = ObjectPackage::WritePackage(Linker, Bytes, Bulk)))
		{ if (OutError) *OutError = ObjectPackage::ToError(Diagnostic); return {}; }
		if (Package->GetPackagePathIdentity() != Data.Identity)
		{ if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::StaleData, "Package changed during capture."}; return {}; }
		const std::string Suffix = ".package-save-" + FGuid::NewGuid().ToString();
		MainFile.Staged = MainFile.Destination.string() + Suffix + ".stage.tmp";
		MainFile.Backup = MainFile.Destination.string() + Suffix + ".backup.tmp";
		BulkFile.Backup = BulkFile.Destination.string() + Suffix + ".backup.tmp";
		if (!Bulk.empty()) BulkFile.Staged = BulkFile.Destination.string() + Suffix + ".stage.tmp";
		std::vector<FPackageWriteFile> Files;
		Data.MainDigest = FXxHash128::HashBuffer(Bytes);
		Data.DetachedBytes = Bytes.size() + Bulk.size();
		Files.push_back({std::move(BulkFile), BulkStamp, std::move(Bulk)});
		Files.push_back({std::move(MainFile), MainStamp, std::move(Bytes)});
		Data.Write = Data.Context.BeginWrite(std::move(Files));
		if (bAsync)
		{
			if (!IsTaskSchedulerRunning())
			{ if (OutError) *OutError = UnifiedError::FError{PackageSaveErrors::ShuttingDown, "Scheduler stopped during preparation."}; return {}; }
			Data.Worker = Tasks::LaunchTask("Package.SaveStaging", [&Data] { Data.Stage(); }, Tasks::ETaskThread::BlockingIO);
		}
		else if (!bDeferStaging) Data.Stage();
		return Operation;
	}
	auto FPackageSaveOperation::IsStagingReady() const -> bool
	{ return !State->Worker.IsValid() || State->Worker.IsCompleted(); }
	auto FPackageSaveOperation::StageDetached() -> void { State->Stage(); }
	auto FPackageSaveOperation::GetDetachedBytes() const -> uint64 { return State->DetachedBytes; }
	auto FPackageSaveOperation::IsCompleted() const -> bool { return State->Result.has_value(); }
	auto FPackageSaveOperation::CommitStaged() -> FPackageWriteResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (Data.bCommitting) return Fail(PackageSaveErrors::Busy, "Commit is already running.");
		if (Data.bCommitted) return {.State = EPackageWriteState::Committed};
		if (!IsStagingReady()) return Fail(PackageSaveErrors::Busy, "Staging is still running.");
		auto FinishFailure = [&](FPackageWriteResult Result) {
			Data.Cleanup(); Data.Result = Result; return Result;
		};
		if (Data.Worker.IsValid())
		{
			if (Data.Worker.GetState() != ETaskState::Succeeded)
				return FinishFailure(Fail(PackageSaveErrors::IoError, "Staging task did not succeed."));
		}
		if (Data.StagingError) return FinishFailure(FPackageWriteResult{.Error = Data.StagingError});
		if (Data.Package->GetPackagePathIdentity() != Data.Identity)
			return FinishFailure(Fail(PackageSaveErrors::StaleData, "Package changed while saving."));
		Data.bCommitting = true;
		auto Result = Data.Write->Commit();
		Data.bCommitting = false;
		if (!Result) return FinishFailure(std::move(Result));
		Data.bCommitted = true;
		return Result;
	}
	auto FPackageSaveOperation::RollbackCommit() -> FPackageWriteResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (!Data.bCommitted) return Data.Result.value_or(FPackageWriteResult{});
		auto Result = Data.Write->Rollback();
		Data.bCommitted = false;
		if (Result) Result = Fail(PackageSaveErrors::Cancelled, "Package commit rolled back.");
		Data.Result = std::move(Result);
		return *Data.Result;
	}
	auto FPackageSaveOperation::FinalizeCommit() -> FPackageWriteResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (!Data.bCommitted) return Fail(PackageSaveErrors::Busy, "No committed package to finalize.");
		Data.Result = Data.Write->Finalize();
		Data.bCommitted = false;
		if (Data.bClearDirty && Data.Package->GetPackagePathIdentity() == Data.Identity)
		{
			Data.Package->RecordDiskExports(std::move(Data.DiskExports), Data.MainDigest);
			Data.Package->ClearDirtyIfRevisionMatches(Data.EditRevision);
			OnPackageSaved().Broadcast(*Data.Package);
		}
		Data.Cleanup();
		return *Data.Result;
	}
	auto FPackageSaveOperation::Complete() -> FPackageWriteResult
	{
		auto Result = CommitStaged();
		return Result && !State->Result ? FinalizeCommit() : Result;
	}
	auto FPackageSaveOperation::WaitAndComplete() -> FPackageWriteResult
	{
		check(IsInGameThread());
		if (State->Worker.IsValid()) State->Worker.Wait();
		return Complete();
	}
	auto FPackageSaveOperation::Cancel() -> FPackageWriteResult
	{
		check(IsInGameThread());
		if (State->Result) return *State->Result;
		if (State->bCommitting || State->bCommitted) return Fail(PackageSaveErrors::Busy, "Commit has already started.");
		if (State->Worker.IsValid()) State->Worker.Wait();
		State->Cleanup();
		State->Result = Fail(PackageSaveErrors::Cancelled, "Save cancelled before publication.");
		return *State->Result;
	}
	auto DPackage::Save(const FPackageSaveOptions& Options) -> FPackageWriteResult
	{
		std::optional<UnifiedError::FError> Error;
		auto Operation = FPackageSaveOperation::Begin(this, Options, &Error, false);
		return Operation ? Operation->WaitAndComplete() : FPackageWriteResult{.Error = std::move(Error)};
	}
	auto FSavePackageContext::SaveAsync(DPackage* Package) const -> Tasks::TTask<FPackageWriteResult>
	{
		if (auto Admission = PackageSavePrivate::CheckAsyncAdmission(); !Admission)
			return Tasks::MakeCompletedTask(FPackageWriteResult{.Error = std::move(Admission.error())});
		std::optional<UnifiedError::FError> Error;
		std::shared_ptr<FPackageSaveOperation> Operation = FPackageSaveOperation::Begin(Package, *this, &Error, false, true);
		if (!Operation) return Tasks::MakeCompletedTask(FPackageWriteResult{.Error = std::move(Error)});
		auto Submitted = PackageSavePrivate::SubmitTypedAsyncSave<FPackageWriteResult>(Operation->GetDetachedBytes(),
			[Operation] { Operation->StageDetached(); },
			[Operation, IsCancellationRequested = Options.IsCancellationRequested](bool bSucceeded) mutable {
				const bool bCancelled = IsCancellationRequested && IsCancellationRequested();
				auto Result = !bSucceeded || bCancelled ? Operation->Cancel() : Operation->Complete();
				if (!bSucceeded) Result = Fail(PackageSaveErrors::IoError, "Package I/O task did not succeed.");
				Operation.reset();
				return Result;
			}, [Operation] { (void)Operation->Cancel(); });
		if (!Submitted) return Tasks::MakeCompletedTask(FPackageWriteResult{.Error = std::move(Submitted.error())});
		return std::move(*Submitted);
	}
}
