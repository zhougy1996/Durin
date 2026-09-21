#include "DObject/PackagePersistence.h"
#include "CoreGlobals.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/FilePublication.h"
#include "Misc/FileHelper.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		FPackageDestinationResolver DestinationResolver;
		std::unordered_set<const DPackage*> PreparingPackages;
		auto Fail(EPackageSaveError Error, std::string Message) -> FPackageSaveResult
		{ return {Error, std::move(Message)}; }
		struct FOwnedAsyncSave
		{
			uint64 Bytes = 0;
			FTaskHandle Disk, Publication;
			std::function<void()> Write;
			std::function<void(bool)> Finish;
			bool bFinishing = false;
		};
		std::mutex AsyncSaveMutex;
		std::vector<std::shared_ptr<FOwnedAsyncSave>> AsyncSaves;
		bool bAcceptAsyncSaves = true;
		bool bDeliveringSave = false;
		PackageSavePrivate::EPublicationFault PublicationFault = PackageSavePrivate::EPublicationFault::None;
		uint32 AsyncSaveMaxOperations = 64;
		uint64 AsyncSaveMaxBytes = 256ull * 1024 * 1024;
		auto SnapshotSaves() -> std::vector<std::shared_ptr<FOwnedAsyncSave>>
		{ std::lock_guard Lock(AsyncSaveMutex); return AsyncSaves; }
		auto FinishAsyncSave(const std::shared_ptr<FOwnedAsyncSave>& Save) -> void
		{
			check(IsInGameThread());
			if (Save->bFinishing || !Save->Finish || !Save->Disk.IsComplete()) return;
			Save->bFinishing = true;
			bDeliveringSave = true;
			Save->Write = {};
			auto Finish = std::move(Save->Finish);
			try { Finish(Save->Disk.GetState() == ETaskState::Succeeded); }
			catch (...) { DURIN_ERROR("Async package save completion threw an exception."); }
			// Even canceled executor work cannot destroy the last live-object owner
			// on an I/O thread. Release all adapter captures before exposing quiescence.
			Finish = {};
			bDeliveringSave = false;
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
			if (bDeliveringSave) return Fail(EPackageSaveError::Busy, "Reentrant async save submission is unsupported.");
			uint64 Retained = 0;
			std::lock_guard Lock(AsyncSaveMutex);
			for (const auto& Save : AsyncSaves) Retained += Save->Bytes;
			if (AsyncSaves.size() >= AsyncSaveMaxOperations || Bytes > AsyncSaveMaxBytes - Retained)
				return Fail(EPackageSaveError::Busy, "Async save capacity is exhausted.");
			return {};
		}
		auto SubmitAsyncSave(uint64 Bytes, std::function<void()> Write, std::function<void(bool)> Finish)
			-> FPackageSaveResult
		{
			if (auto Result = CheckAsyncAdmission(Bytes); !Result) return Result;
			auto Save = std::make_shared<FOwnedAsyncSave>();
			Save->Bytes = Bytes; Save->Write = std::move(Write); Save->Finish = std::move(Finish);
			// Register completion before releasing disk work. Rejected deferred
			// admission must never allow a destructive write to start.
			auto Gate = Tasks::TCompletionSource<void>::Create({.DebugName = "Package.SaveAdmission"});
			const std::array Prerequisites{Gate.GetCompletion().GetTaskHandle()};
			FTaskLaunchOptions DiskOptions;
			DiskOptions.Target = ETaskTarget::BlockingIO;
			DiskOptions.bQueueOnSaturation = true;
			DiskOptions.Prerequisites = Prerequisites;
			auto Disk = Private::TryLaunchCancelableTaskWithCompletion("Package.SaveIO",
				[Save](const FTaskCancellationToken&) { Save->Write(); }, [](ETaskState) {}, DiskOptions);
			if (!Disk.HasValue())
			{ Gate.TrySetCanceled(); return Fail(EPackageSaveError::ShuttingDown, "Save I/O admission failed."); }
			Save->Disk = std::move(Disk).TakeValue();
			if (PublicationFault == EPublicationFault::RejectAdmission)
			{
				Gate.TrySetCanceled();
				(void)WaitTask(Save->Disk);
				Save->Write = {}; Save->Finish = {};
				return Fail(EPackageSaveError::ShuttingDown, "Injected save publication admission failure.");
			}
			FTaskContinuationOptions CompletionOptions;
			CompletionOptions.Target = ETaskTarget::GameThreadDeferred;
			CompletionOptions.bQueueOnSaturation = true;
			auto Completion = Private::TryLaunchContinuationTask(Save->Disk, "Package.SavePublication",
				[Save](const FTaskCancellationToken&) { FinishAsyncSave(Save); }, [](ETaskState) {},
				CompletionOptions, ETaskDependencyKind::Completion);
			if (!Completion.HasValue())
			{
				Gate.TrySetCanceled();
				(void)WaitTask(Save->Disk);
				Save->Write = {}; Save->Finish = {};
				return Fail(EPackageSaveError::ShuttingDown, "Save publication admission failed.");
			}
			Save->Publication = std::move(Completion).TakeValue();
			{ std::lock_guard Lock(AsyncSaveMutex); AsyncSaves.push_back(Save); }
			if (PublicationFault == EPublicationFault::CancelContinuation) CancelTask(Save->Publication);
			Gate.TrySetValue();
			return {};
		}
		auto PollAsyncSaves() -> void
		{
			check(IsInGameThread());
			if (bDeliveringSave) return;
			for (const auto& Save : SnapshotSaves())
				if (Save->Disk.IsComplete() && Save->Publication.IsComplete()) FinishAsyncSave(Save);
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
		for (const auto& Save : SnapshotSaves()) if (!Save->Disk.IsComplete()) return true;
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
		if (!GIsGameThreadIdInitialized || !IsInGameThread() || bDeliveringSave)
			return Fail(EPackageSaveError::Busy, "Save publication drain requires a non-reentrant GameThread caller.");
		for (const auto& Save : SnapshotSaves())
		{
			if (WaitTask(Save->Disk).WaitStatus != ETaskWaitStatus::Completed)
				return Fail(EPackageSaveError::Busy, "Save I/O wait is unsupported on this thread.");
			CancelTask(Save->Publication);
			FinishAsyncSave(Save);
		}
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
		return {Error, std::move(Result.Message), State, std::move(Result.RecoveryFiles), std::move(Result.AffectedFiles)};
	}
	struct FPackageSaveOperation::FState
	{
		TStrongObjectPtr<DPackage> Package;
		FPackagePath Identity;
		uint64 Revision = 0;
		uint64 DetachedBytes = 0;
		FSavePackageContext Context;
		std::unique_ptr<IPackageWriteOperation> Write;
		std::unique_ptr<Tasks::FTaskGroup> Group;
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
			Admission = Fail(GetPackageCaptureSaveError(CaptureResult.Error), FormatPackageCaptureError(CaptureResult.Error));
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
			Data.Group = std::make_unique<Tasks::FTaskGroup>();
			Data.Worker = Tasks::LaunchTask(*Data.Group, Tasks::ETaskExecutor::BlockingIO,
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
			if (Data.Worker.GetCompletion().GetState() != ETaskState::Succeeded)
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
		auto Source = Tasks::TCompletionSource<FPackageSaveResult>::Create({.DebugName = "Package.SaveAsync"});
		auto Task = Source.TakeTask();
		Admission = PackageSavePrivate::SubmitAsyncSave(Operation->GetDetachedBytes(),
			[Operation] { Operation->StageDetached(); },
			[Operation, Source, Cancellation = Options.Cancellation](bool bSucceeded) mutable {
				const bool bCancelled = Cancellation.IsCancellationRequested()
					|| Private::FTaskRuntimeAccess::IsCancellationRequested(Source.GetCompletion().GetTaskHandle());
				auto Result = !bSucceeded || bCancelled ? Operation->Cancel() : Operation->Complete();
				if (!bSucceeded) Result = Fail(EPackageSaveError::IoError, "Package I/O task did not succeed.");
				Operation.reset();
				Source.TrySetValue(std::move(Result));
			});
		if (!Admission) { Source.TrySetValue(Admission); return {}; }
		return Task;
	}
}
