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
			: EPackageCommitState::NotCommitted;
		return {Error, std::move(Result.Message), State, std::move(Result.RecoveryFiles)};
	}
	struct FPackageSaveOperation::FState
	{
		TStrongObjectPtr<DPackage> Package;
		FPackagePath Identity;
		uint64 Revision = 0;
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
		FPackageSaveResult& Admission, bool bAsync) -> std::unique_ptr<FPackageSaveOperation>
	{
		const auto& Options = Context.Options;
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
		BulkFile.Destination = MainFile.Destination;
		BulkFile.Destination.replace_extension(".dbulk");
		if (!FFilePublicationStamp::Inspect(MainFile.Destination, MainStamp)
			|| !FFilePublicationStamp::Inspect(BulkFile.Destination, BulkStamp))
		{ Admission = Fail(EPackageSaveError::IoError, "Cannot inspect package destination."); return {}; }
		ObjectPackage::FLinkerTables Linker;
		Admission = Data.Context.Capture(Package, Linker);
		if (!Admission) return {};
		ObjectPackage::FPackageWriterDiagnostic Diagnostic;
		if (!ObjectPackage::WritePackage(Linker, Bytes, Bulk, &Diagnostic))
		{ Admission = Fail(EPackageSaveError::UnsupportedProperty, Diagnostic.Message); return {}; }
		if (Package->GetEditRevision() != Data.Revision || Package->GetPackagePathIdentity() != Data.Identity)
		{ Admission = Fail(EPackageSaveError::StaleData, "Package changed during capture."); return {}; }
		const std::string Suffix = ".package-save-" + FGuid::NewGuid().ToString();
		MainFile.Staged = MainFile.Destination.string() + Suffix;
		MainFile.Backup = MainFile.Staged.string() + ".backup";
		BulkFile.Backup = BulkFile.Destination.string() + Suffix + ".backup";
		if (!Bulk.empty()) BulkFile.Staged = BulkFile.Destination.string() + Suffix;
		std::vector<FPackageWriteFile> Files;
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
	auto DPackage::SaveAsync(FPackageSaveResult& Admission, const FPackageSaveOptions& Options)
		-> std::unique_ptr<FPackageSaveOperation>
	{ return FPackageSaveOperation::Begin(this, Options, Admission); }
}
