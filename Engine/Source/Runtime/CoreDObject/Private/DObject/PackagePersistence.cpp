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
			std::function<void()> Write, Cleanup;
		};
	}
	namespace
	{
		FPackageDestinationResolver DestinationResolver;
		std::unordered_set<const DPackage*> PreparingPackages;
		auto Fail(EPackageSaveError Error, std::string Message) -> FPackageSaveResult
		{ return {Error, std::move(Message)}; }
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
			Save->Write = {};
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
		auto CheckAsyncAdmission(uint64 Bytes) -> FPackageSaveResult
		{
			check(IsInGameThread());
			if (!bAcceptAsyncSaves || !IsTaskSchedulerRunning()
				|| !GetGameThreadDeferredWorkQueueDiagnostics().bAccepting)
				return Fail(EPackageSaveError::ShuttingDown, "Async saves require an accepting GameThread executor.");
			if (SavePublicationDepth != 0 || bDrainingSaves) return Fail(EPackageSaveError::Busy, "Async save submission during publication or drain is unsupported.");
			uint64 Retained = 0;
			std::lock_guard Lock(AsyncSaveMutex);
			for (const auto& Save : AsyncSaves) Retained += Save->Bytes;
			if (AsyncSaves.size() >= AsyncSaveMaxOperations || Bytes > AsyncSaveMaxBytes - Retained)
				return Fail(EPackageSaveError::Busy, "Async save capacity is exhausted.");
			return {};
		}
		auto BeginAsyncSavePublication(const std::shared_ptr<FAsyncSaveState>& Save) -> void
		{
			check(IsInGameThread());
			check(Save->Disk.IsCompleted());
			++SavePublicationDepth;
			Save->Write = {};
		}
		auto EndAsyncSavePublication() -> void { check(IsInGameThread() && SavePublicationDepth != 0); --SavePublicationDepth; }

		auto SubmitAsyncSaveTask(uint64 Bytes, std::function<void()> Write,
			FAsyncSavePublicationFactory MakePublication, std::function<void()> Cleanup) -> FPackageSaveResult
		{
			if (auto Result = CheckAsyncAdmission(Bytes); !Result) return Result;
			// Reserve ownership before creating accepted work; registration below cannot allocate.
			{ std::lock_guard Lock(AsyncSaveMutex); AsyncSaves.reserve(AsyncSaves.size() + 1); }
			auto Save = std::make_shared<FAsyncSaveState>();
			Save->Bytes = Bytes; Save->Write = std::move(Write); Save->Cleanup = std::move(Cleanup);
			Tasks::FTaskEvent Gate("Package.SaveAdmission");
			try
			{
				Save->Disk = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO,
					{.DebugName = "Package.SaveIO"}, [Save] {
						if (!Save->bAdmitted) {
							Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
							return;
						}
						Save->Write();
					}, Gate);
				if (PublicationFault == EPublicationFault::RejectAdmission)
				{
					Save->Write = {}; Save->Cleanup = {};
					Gate.Trigger();
					return Fail(EPackageSaveError::ShuttingDown, "Injected save publication admission failure.");
				}
				Save->Publication = MakePublication(Save->Disk, Save);
				{ std::lock_guard Lock(AsyncSaveMutex); AsyncSaves.push_back(Save); }
				if (PublicationFault == EPublicationFault::CancelContinuation) CancelTask(Save->Publication);
				Save->bAdmitted = true;
				Gate.Trigger();
				return {};
			}
			catch (...)
			{
				Save->Write = {}; Save->Cleanup = {};
				Gate.Trigger();
				throw;
			}
		}
		auto SubmitAsyncSave(uint64 Bytes, std::function<void()> Write, std::function<void(bool)> Finish)
			-> FPackageSaveResult
		{
			FPackageSaveResult Admission;
			(void)SubmitTypedAsyncSave<void>(Bytes, std::move(Write),
				[Finish = std::move(Finish)](bool bSucceeded, const FTaskCancellationToken&) { Finish(bSucceeded); }, Admission);
			return Admission;
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
			auto Result = WaitTask(Save->Disk);
			if (Result.WaitStatus != ETaskWaitStatus::Completed) return Result;
			if (Result.TaskState == ETaskState::Failed) State = ETaskState::Failed;
			else if (Result.TaskState == ETaskState::Canceled && State != ETaskState::Failed) State = ETaskState::Canceled;
		}
		return {ETaskWaitStatus::Completed, State};
	}
	auto DPackage::DrainAsyncSaves() -> FPackageSaveResult
	{
		// Configuration-only tools may shut down before creating a GameThread.
		if (!GIsGameThreadIdInitialized && SnapshotSaves().empty()) return {};
		if (!GIsGameThreadIdInitialized || !IsInGameThread() || SavePublicationDepth != 0 || bDrainingSaves)
			return Fail(EPackageSaveError::Busy, "Save publication drain requires a non-reentrant GameThread caller.");
		bDrainingSaves = true;
		struct FScope { ~FScope() { bDrainingSaves = false; } } Scope;
		const auto Cutoff = SnapshotSaves();
		for (const auto& Save : Cutoff)
			if (WaitTask(Save->Disk).WaitStatus != ETaskWaitStatus::Completed)
				return Fail(EPackageSaveError::Busy, "Save I/O wait is unsupported on this thread.");
		for (const auto& Save : Cutoff)
		{
			while (!Save->Publication.IsCompleted())
			{
				const auto Before = GetGameThreadDeferredWorkQueueDiagnostics();
				if (!Before.bInstalled) return Fail(EPackageSaveError::Busy, "Save publication executor is unavailable.");
				const auto Pumped = PumpGameThreadDeferredWork({.MaxCallbacks = 1});
				if (!Pumped.ExecutedCallbacks && !Pumped.TerminalEntriesSkipped
					&& GetGameThreadDeferredWorkQueueDiagnostics().ReentrantPumpCount != Before.ReentrantPumpCount)
					return Fail(EPackageSaveError::Busy, "Cannot drain saves from a deferred callback.");
				if (!Pumped.ExecutedCallbacks && !Pumped.TerminalEntriesSkipped) std::this_thread::yield();
			}
		}
		PackageSavePrivate::PollAsyncSaves();
		return {};
	}
	auto SetPackageDestinationResolver(FPackageDestinationResolver Resolver) -> void
	{
		check(IsInGameThread());
		DestinationResolver = std::move(Resolver);
	}
	auto ToPackageSaveResult(FPackageWriteResult Result) -> FPackageSaveResult
	{
		EPackageSaveError Error = EPackageSaveError::None;
		switch (Result.Error)
		{
		case EPackageWriteError::None: break;
		case EPackageWriteError::IoError: Error = EPackageSaveError::IoError; break;
		case EPackageWriteError::StaleData: Error = EPackageSaveError::StaleData; break;
		case EPackageWriteError::CorruptFile: Error = EPackageSaveError::CorruptFile; break;
		case EPackageWriteError::InvalidState: Error = EPackageSaveError::Busy; break;
		}
		const auto State = Result.State == EPackageWriteState::Committed ? EPackageCommitState::Committed
			: Result.State == EPackageWriteState::RecoveryRequired ? EPackageCommitState::RecoveryRequired
			: Result.State == EPackageWriteState::PartiallyWritten ? EPackageCommitState::PartiallyWritten
			: EPackageCommitState::NotCommitted;
		return {Error, std::move(Result.Message), State, std::move(Result.RecoveryFiles), std::move(Result.AffectedFiles), std::move(Result.FileCause)};
	}
	struct FPackageSaveOperation::FState
	{
		TStrongObjectPtr<DPackage> Package;
		FPackagePath Identity;
		uint64 Revision = 0;
		uint64 DetachedBytes = 0;
		FSavePackageContext Context;
		std::unique_ptr<IPackageWriteOperation> Write;
		Tasks::TTask<FPackageSaveResult> Worker;
		FPackageSaveResult StagingResult;
		std::optional<FPackageSaveResult> Result;
		bool bCommitted = false;
		bool bCommitting = false;
		bool bClearDirty = true;
		auto Stage() -> FPackageSaveResult { return ToPackageSaveResult(Write->Stage()); }
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
		FPackageSaveResult& Admission, bool bAsync) -> std::unique_ptr<FPackageSaveOperation>
	{
		return Begin(Package, FSavePackageContext{Options}, Admission, bAsync);
	}
	auto FPackageSaveOperation::Begin(DPackage* Package, const FSavePackageContext& Context,
		FPackageSaveResult& Admission, bool bAsync, bool bDeferStaging) -> std::unique_ptr<FPackageSaveOperation>
	{
		const auto& Options = Context.Options;
		check(IsInGameThread());
		Admission = {};
		if (Options.Flags != SAVE_None)
		{ Admission = Fail(EPackageSaveError::InvalidPackageType, "Protected saving does not accept direct-write flags."); return {}; }
		if (Context.Writer && !Context.Writer->SupportsRollback())
		{ Admission = Fail(EPackageSaveError::InvalidPackageType, "Protected saves require a transactional writer."); return {}; }
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
		{ Admission = Fail(EPackageSaveError::InvalidPackageType, "A live persistent package is required."); return {}; }
		if (bAsync && !IsTaskSchedulerRunning())
		{ Admission = Fail(EPackageSaveError::ShuttingDown, "The task scheduler is not running."); return {}; }
		if (!PreparingPackages.insert(Package).second)
		{ Admission = Fail(EPackageSaveError::Busy, "Package preparation is already running."); return {}; }
		struct FPreparationGuard
		{
			const DPackage* Package;
			~FPreparationGuard() { PreparingPackages.erase(Package); }
		} Guard{Package};
		auto Operation = std::unique_ptr<FPackageSaveOperation>(new FPackageSaveOperation());
		auto& Data = *Operation->State;
		Data.Package = Package;
		Data.Context = Context;
		FFileReplacement MainFile, BulkFile;
		FFilePublicationStamp MainStamp, BulkStamp;
		FByteBuffer Bytes, Bulk;
		Data.Identity = Package->GetPackagePathIdentity();
		Data.Revision = Package->GetEditRevision();
		Data.bClearDirty = !Options.Capture.bCooking && !Options.Capture.PropertyFilter
			&& (!Options.Capture.SaveOverrides || Options.Capture.SaveOverrides->IsEmpty());
		auto Resolver = DestinationResolver;
		MainFile.Destination = Options.Destination.empty() && Resolver ? Resolver(*Package) : Options.Destination;
		if (MainFile.Destination.empty() || MainFile.Destination.extension() != ".dasset")
		{ Admission = Fail(EPackageSaveError::InvalidPath, "An explicit .dasset destination or configured resolver is required."); return {}; }
		std::error_code Ec;
		MainFile.Destination = std::filesystem::absolute(MainFile.Destination, Ec).lexically_normal();
		if (Ec) { Admission = Fail(EPackageSaveError::InvalidPath, Ec.message()); return {}; }
		auto ReadAccess = FPackageFileAccess::TryReadPackage(MainFile.Destination);
		if (!ReadAccess) { Admission = Fail(EPackageSaveError::Busy, "Package output is being written."); return {}; }
		BulkFile.Destination = MainFile.Destination;
		BulkFile.Destination.replace_extension(".dbulk");
		if (!FFilePublicationStamp::Inspect(MainFile.Destination, MainStamp)
			|| !FFilePublicationStamp::Inspect(BulkFile.Destination, BulkStamp))
		{ Admission = Fail(EPackageSaveError::IoError, "Cannot inspect package destination."); return {}; }
		ObjectPackage::FLinkerTables Linker;
		const auto CaptureResult = Data.Context.Capture(Package, Linker);
		if (!CaptureResult)
		{
			Admission = Fail(GetPackageCaptureSaveError(CaptureResult.error()), ToString(CaptureResult.error()));
			Admission.CaptureCause = CaptureResult.error();
			return {};
		}
		ObjectPackage::FPackageWriterResult Diagnostic;
		if (!(Diagnostic = ObjectPackage::WritePackage(Linker, Bytes, Bulk)))
		{ Admission = Fail(EPackageSaveError::UnsupportedProperty, Durin::ObjectPackage::FormatPackageError(Diagnostic)); return {}; }
		if (Package->GetEditRevision() != Data.Revision || Package->GetPackagePathIdentity() != Data.Identity)
		{ Admission = Fail(EPackageSaveError::StaleData, "Package changed during capture."); return {}; }
		const std::string Suffix = ".package-save-" + FGuid::NewGuid().ToString();
		MainFile.Staged = MainFile.Destination.string() + Suffix + ".stage.tmp";
		MainFile.Backup = MainFile.Destination.string() + Suffix + ".backup.tmp";
		BulkFile.Backup = BulkFile.Destination.string() + Suffix + ".backup.tmp";
		if (!Bulk.empty()) BulkFile.Staged = BulkFile.Destination.string() + Suffix + ".stage.tmp";
		std::vector<FPackageWriteFile> Files;
		Data.DetachedBytes = Bytes.size() + Bulk.size();
		Files.push_back({std::move(BulkFile), BulkStamp, std::move(Bulk)});
		Files.push_back({std::move(MainFile), MainStamp, std::move(Bytes)});
		Data.Write = Data.Context.BeginWrite(std::move(Files));
		if (bAsync)
		{
			if (!IsTaskSchedulerRunning())
			{ Admission = Fail(EPackageSaveError::ShuttingDown, "Scheduler stopped during preparation."); return {}; }
			Data.Worker = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO,
				{.DebugName = "Package.SaveStaging"}, [&Data] { return Data.Stage(); });
		}
		else if (!bDeferStaging) Data.StagingResult = Data.Stage();
		return Operation;
	}
	auto FPackageSaveOperation::IsStagingReady() const -> bool
	{ return !State->Worker.IsValid() || State->Worker.IsCompleted(); }
	auto FPackageSaveOperation::StageDetached() -> void { State->StagingResult = State->Stage(); }
	auto FPackageSaveOperation::GetDetachedBytes() const -> uint64 { return State->DetachedBytes; }
	auto FPackageSaveOperation::IsCompleted() const -> bool { return State->Result.has_value(); }
	auto FPackageSaveOperation::CommitStaged() -> FPackageSaveResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (Data.bCommitting) return Fail(EPackageSaveError::Busy, "Commit is already running.");
		if (Data.bCommitted) return {EPackageSaveError::None, {}, EPackageCommitState::Committed};
		if (!IsStagingReady()) return Fail(EPackageSaveError::Busy, "Staging is still running.");
		auto FinishFailure = [&](FPackageSaveResult Result) {
			Data.Cleanup(); Data.Result = Result; return Result;
		};
		if (Data.Worker.IsValid())
		{
			if (Data.Worker.GetState() != ETaskState::Succeeded)
				return FinishFailure(Fail(EPackageSaveError::IoError, "Staging task did not succeed."));
			Data.StagingResult = Data.Worker.GetResult();
		}
		if (!Data.StagingResult) return FinishFailure(Data.StagingResult);
		if (Data.Package->GetEditRevision() != Data.Revision
			|| Data.Package->GetPackagePathIdentity() != Data.Identity)
			return FinishFailure(Fail(EPackageSaveError::StaleData, "Package changed while saving."));
		Data.bCommitting = true;
		auto Result = ToPackageSaveResult(Data.Write->Commit());
		Data.bCommitting = false;
		if (!Result) return FinishFailure(std::move(Result));
		Data.bCommitted = true;
		return Result;
	}
	auto FPackageSaveOperation::RollbackCommit() -> FPackageSaveResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (!Data.bCommitted) return Data.Result.value_or(FPackageSaveResult{});
		auto Result = ToPackageSaveResult(Data.Write->Rollback());
		Data.bCommitted = false;
		if (Result) Result = Fail(EPackageSaveError::Cancelled, "Package commit rolled back.");
		Data.Result = std::move(Result);
		return *Data.Result;
	}
	auto FPackageSaveOperation::FinalizeCommit() -> FPackageSaveResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (!Data.bCommitted) return Fail(EPackageSaveError::Busy, "No committed package to finalize.");
		Data.Result = ToPackageSaveResult(Data.Write->Finalize());
		Data.bCommitted = false;
		if (Data.bClearDirty && Data.Package->GetEditRevision() == Data.Revision
			&& Data.Package->GetPackagePathIdentity() == Data.Identity) Data.Package->ClearDirty();
		Data.Cleanup();
		return *Data.Result;
	}
	auto FPackageSaveOperation::Complete() -> FPackageSaveResult
	{
		auto Result = CommitStaged();
		return Result && !State->Result ? FinalizeCommit() : Result;
	}
	auto FPackageSaveOperation::WaitAndComplete() -> FPackageSaveResult
	{
		check(IsInGameThread());
		if (State->Worker.IsValid()) State->Worker.Wait();
		return Complete();
	}
	auto FPackageSaveOperation::Cancel() -> FPackageSaveResult
	{
		check(IsInGameThread());
		if (State->Result) return *State->Result;
		if (State->bCommitting || State->bCommitted) return Fail(EPackageSaveError::Busy, "Commit has already started.");
		if (State->Worker.IsValid()) State->Worker.Wait();
		State->Cleanup();
		State->Result = Fail(EPackageSaveError::Cancelled, "Save cancelled before publication.");
		return *State->Result;
	}
	auto DPackage::Save(const FPackageSaveOptions& Options) -> FPackageSaveResult
	{
		FPackageSaveResult Admission;
		auto Operation = FPackageSaveOperation::Begin(this, Options, Admission, false);
		return Operation ? Operation->WaitAndComplete() : Admission;
	}
	auto FSavePackageContext::SaveAsync(DPackage* Package, FPackageSaveResult& Admission) const
		-> Tasks::TTask<FPackageSaveResult>
	{
		Admission = PackageSavePrivate::CheckAsyncAdmission();
		if (!Admission) return {};
		std::shared_ptr<FPackageSaveOperation> Operation = FPackageSaveOperation::Begin(Package, *this, Admission, false, true);
		if (!Operation) return {};
		Admission = PackageSavePrivate::CheckAsyncAdmission(Operation->GetDetachedBytes());
		if (!Admission) return {};
		return PackageSavePrivate::SubmitTypedAsyncSave<FPackageSaveResult>(Operation->GetDetachedBytes(),
			[Operation] { Operation->StageDetached(); },
			[Operation, Cancellation = Options.Cancellation](bool bSucceeded, const FTaskCancellationToken& Token) mutable {
				const bool bCancelled = Cancellation.IsCancellationRequested() || Token.IsCancellationRequested();
				auto Result = !bSucceeded || bCancelled ? Operation->Cancel() : Operation->Complete();
				if (!bSucceeded) Result = Fail(EPackageSaveError::IoError, "Package I/O task did not succeed.");
				Operation.reset();
				return Result;
			}, Admission);
	}
}
