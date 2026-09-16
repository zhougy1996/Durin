#include "DObject/PackagePersistence.h"
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
	}
	auto SetPackageDestinationResolver(FPackageDestinationResolver Resolver) -> void
	{
		check(IsInGameThread());
		DestinationResolver = std::move(Resolver);
	}
	struct FPackageSaveOperation::FState
	{
		TStrongObjectPtr<DPackage> Package;
		FPackagePath Identity;
		uint64 Revision = 0;
		FByteBuffer Bytes, Bulk;
		FFileReplacement MainFile, BulkFile;
		FFilePublicationStamp MainStamp, BulkStamp;
		std::unique_ptr<Tasks::FTaskGroup> Group;
		Tasks::TTask<FPackageSaveResult> Worker;
		FPackageSaveResult StagingResult;
		std::optional<FPackageSaveResult> Result;
		bool bCommitted = false;
		bool bCommitting = false;
		bool bClearDirty = true;
		auto Stage() -> FPackageSaveResult
		{
			std::error_code Ec;
			std::filesystem::create_directories(MainFile.Destination.parent_path(), Ec);
			if (Ec) return Fail(EPackageSaveError::IoError, Ec.message());
			std::string Error;
			if (!StageFileVerified(MainFile.Staged, Bytes, Error)
				|| (!Bulk.empty() && !StageFileVerified(BulkFile.Staged, Bulk, Error)))
				return Fail(EPackageSaveError::IoError, Error);
			return {};
		}
		auto Cleanup() -> void
		{
			std::error_code Ec;
			if (!MainFile.Staged.empty()) std::filesystem::remove(MainFile.Staged, Ec);
			if (!BulkFile.Staged.empty()) std::filesystem::remove(BulkFile.Staged, Ec);
		}
		auto VerifyStage(const FFileReplacement& File, FByteView Expected) -> bool
		{
			if (File.Staged.empty()) return Expected.empty();
			FByteBuffer Read;
			return FFileHelper::LoadFileToArray(Read, File.Staged) && Read.size() == Expected.size()
				&& FXxHash128::HashBuffer(Read) == FXxHash128::HashBuffer(Expected);
		}
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
		check(IsInGameThread());
		Admission = {};
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
		Data.Identity = Package->GetPackagePathIdentity();
		Data.Revision = Package->GetEditRevision();
		Data.bClearDirty = !Options.Capture.bCooking && !Options.Capture.PropertyFilter
			&& (!Options.Capture.SaveOverrides || Options.Capture.SaveOverrides->IsEmpty());
		auto Resolver = DestinationResolver;
		Data.MainFile.Destination = Options.Destination.empty() && Resolver ? Resolver(*Package) : Options.Destination;
		if (Data.MainFile.Destination.empty() || Data.MainFile.Destination.extension() != ".dasset")
		{ Admission = Fail(EPackageSaveError::InvalidPath, "An explicit .dasset destination or configured resolver is required."); return {}; }
		std::error_code Ec;
		Data.MainFile.Destination = std::filesystem::absolute(Data.MainFile.Destination, Ec).lexically_normal();
		if (Ec) { Admission = Fail(EPackageSaveError::InvalidPath, Ec.message()); return {}; }
		Data.BulkFile.Destination = Data.MainFile.Destination;
		Data.BulkFile.Destination.replace_extension(".dbulk");
		if (!FFilePublicationStamp::Inspect(Data.MainFile.Destination, Data.MainStamp)
			|| !FFilePublicationStamp::Inspect(Data.BulkFile.Destination, Data.BulkStamp))
		{ Admission = Fail(EPackageSaveError::IoError, "Cannot inspect package destination."); return {}; }
		ObjectPackage::FLinkerTables Linker;
		Admission = CapturePackageLinker(Package, Options.Mode == EPackageSaveMode::Complete
			? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, Options.Capture, Linker);
		if (!Admission) return {};
		ObjectPackage::FPackageWriterDiagnostic Diagnostic;
		if (!ObjectPackage::WritePackage(Linker, Data.Bytes, Data.Bulk, &Diagnostic))
		{ Admission = Fail(EPackageSaveError::UnsupportedProperty, Diagnostic.Message); return {}; }
		if (Package->GetEditRevision() != Data.Revision || Package->GetPackagePathIdentity() != Data.Identity)
		{ Admission = Fail(EPackageSaveError::StaleData, "Package changed during capture."); return {}; }
		const std::string Suffix = ".package-save-" + FGuid::NewGuid().ToString();
		Data.MainFile.Staged = Data.MainFile.Destination.string() + Suffix;
		Data.MainFile.Backup = Data.MainFile.Staged.string() + ".backup";
		Data.BulkFile.Backup = Data.BulkFile.Destination.string() + Suffix + ".backup";
		if (!Data.Bulk.empty()) Data.BulkFile.Staged = Data.BulkFile.Destination.string() + Suffix;
		if (bAsync)
		{
			if (!IsTaskSchedulerRunning())
			{ Admission = Fail(EPackageSaveError::ShuttingDown, "Scheduler stopped during preparation."); return {}; }
			Data.Group = std::make_unique<Tasks::FTaskGroup>();
			Data.Worker = Tasks::LaunchTask(*Data.Group, Tasks::ETaskExecutor::BlockingIO,
				{.DebugName = "Package.SaveStaging"}, [&Data] { return Data.Stage(); });
		}
		else Data.StagingResult = Data.Stage();
		return Operation;
	}
	auto FPackageSaveOperation::IsStagingReady() const -> bool
	{ return !State->Worker.IsValid() || State->Worker.IsCompleted(); }
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
		FFilePublicationStamp Main, Bulk;
		if (Data.Package->GetEditRevision() != Data.Revision
			|| Data.Package->GetPackagePathIdentity() != Data.Identity
			|| !FFilePublicationStamp::Inspect(Data.MainFile.Destination, Main) || Main != Data.MainStamp
			|| !FFilePublicationStamp::Inspect(Data.BulkFile.Destination, Bulk) || Bulk != Data.BulkStamp)
			return FinishFailure(Fail(EPackageSaveError::StaleData, "Package or destination changed while saving."));
		if (!Data.VerifyStage(Data.MainFile, Data.Bytes) || !Data.VerifyStage(Data.BulkFile, Data.Bulk))
			return FinishFailure(Fail(EPackageSaveError::CorruptFile, "Staged package content changed."));
		Data.bCommitting = true;
		std::string Error;
		if (!Data.BulkFile.Publish(Error) || !Data.MainFile.Publish(Error))
		{
			std::string RestoreError, BulkRestoreError;
			const bool MainRestored = Data.MainFile.Rollback(RestoreError);
			const bool BulkRestored = Data.BulkFile.Rollback(BulkRestoreError);
			if (!BulkRestored) RestoreError += "; bulk: " + BulkRestoreError;
			Data.bCommitting = false;
			auto Result = Fail(EPackageSaveError::IoError, Error);
			if (!MainRestored || !BulkRestored)
			{ Result.CommitState = EPackageCommitState::RecoveryRequired; Result.Message += "; rollback: " + RestoreError;
				if (Data.MainFile.bBackedUp) Result.RecoveryFiles.push_back(Data.MainFile.Backup);
				if (Data.BulkFile.bBackedUp) Result.RecoveryFiles.push_back(Data.BulkFile.Backup); }
			return FinishFailure(Result);
		}
		Data.bCommitted = true;
		Data.bCommitting = false;
		return {EPackageSaveError::None, {}, EPackageCommitState::Committed};
	}
	auto FPackageSaveOperation::RollbackCommit() -> FPackageSaveResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (!Data.bCommitted) return Data.Result.value_or(FPackageSaveResult{});
		std::string Error, BulkError;
		const bool Main = Data.MainFile.Rollback(Error);
		const bool Bulk = Data.BulkFile.Rollback(BulkError);
		if (!Bulk) Error += "; bulk: " + BulkError;
		Data.bCommitted = false;
		Data.Result = FPackageSaveResult{EPackageSaveError::Cancelled, "Package commit rolled back."};
		if (!Main || !Bulk) Data.Result = FPackageSaveResult{EPackageSaveError::IoError,
			"Rollback failed: " + Error, EPackageCommitState::RecoveryRequired};
		if (Data.MainFile.bBackedUp) Data.Result->RecoveryFiles.push_back(Data.MainFile.Backup);
		if (Data.BulkFile.bBackedUp) Data.Result->RecoveryFiles.push_back(Data.BulkFile.Backup);
		return *Data.Result;
	}
	auto FPackageSaveOperation::FinalizeCommit() -> FPackageSaveResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (!Data.bCommitted) return Fail(EPackageSaveError::Busy, "No committed package to finalize.");
		std::string Error, BulkError;
		const bool Main = Data.MainFile.Finalize(Error);
		const bool Bulk = Data.BulkFile.Finalize(BulkError);
		if (!Bulk) Error += "; bulk: " + BulkError;
		Data.bCommitted = false;
		if (Data.bClearDirty && Data.Package->GetEditRevision() == Data.Revision
			&& Data.Package->GetPackagePathIdentity() == Data.Identity) Data.Package->ClearDirty();
		Data.Result = FPackageSaveResult{Main && Bulk ? EPackageSaveError::None : EPackageSaveError::IoError,
			Error, EPackageCommitState::Committed};
		if (Data.MainFile.bBackedUp) Data.Result->RecoveryFiles.push_back(Data.MainFile.Backup);
		if (Data.BulkFile.bBackedUp) Data.Result->RecoveryFiles.push_back(Data.BulkFile.Backup);
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
	auto DPackage::SaveAsync(FPackageSaveResult& Admission, const FPackageSaveOptions& Options)
		-> std::unique_ptr<FPackageSaveOperation>
	{ return FPackageSaveOperation::Begin(this, Options, Admission); }
}
