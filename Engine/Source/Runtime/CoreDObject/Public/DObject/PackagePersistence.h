#pragma once
#include "DObject/PackageCapture.h"
#include "Misc/PackageWriter.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	enum EPackageSaveFlags : uint32 { SAVE_None = 0, SAVE_Async = 1 };
	namespace PackageSavePrivate
	{
		// Internal detached-work owner shared by file-only and Engine adapters.
		COREDOBJECT_API auto CheckAsyncAdmission(uint64 Bytes = 0) -> FPackageSaveResult;
		COREDOBJECT_API auto SubmitAsyncSave(uint64 Bytes, std::function<void()> Write,
			std::function<void(bool)> Finish) -> FPackageSaveResult;
		COREDOBJECT_API auto PollAsyncSaves() -> void;
		COREDOBJECT_API auto SetAsyncSaveAdmission(bool bAccepting) -> void;
		enum class EPublicationFault : uint8 { None, RejectAdmission, CancelContinuation };
		COREDOBJECT_API auto SetAsyncSavePublicationFaultForTests(EPublicationFault Fault) -> void;
		COREDOBJECT_API auto SetAsyncSaveLimitsForTests(uint32 MaxOperations, uint64 MaxBytes) -> void;
	}
	enum class EPackageSaveMode : uint8 { Delta, Complete };
	struct FPackageSaveOptions
	{
		// Explicit physical destination; an empty path uses the configured resolver.
		std::filesystem::path Destination;
		EPackageSaveMode Mode = EPackageSaveMode::Delta;
		FPackageCaptureOptions Capture;
		EPackageSaveFlags Flags = SAVE_None;
		FTaskCancellationToken Cancellation;
	};
	struct FSavePackageContext
	{
		FPackageSaveOptions Options;
		std::shared_ptr<IPackageWriter> Writer = GetFilePackageWriter();
		COREDOBJECT_API auto SaveAsync(DPackage* Package, FPackageSaveResult& Admission) const
			-> Tasks::TTask<FPackageSaveResult>;
		auto Capture(DPackage* Package, ObjectPackage::FLinkerTables& OutLinker,
			uint32 FormatVersion = ObjectPackage::DastV10FormatVersion) const
			-> FPackageCaptureResult
		{
			return CapturePackageLinker(Package, Options.Mode == EPackageSaveMode::Complete
				? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, Options.Capture, OutLinker, FormatVersion);
		}
		// Detached callers supply file order and recovery paths; ownership transfers.
		auto BeginWrite(std::vector<FPackageWriteFile> Files) const
			-> std::unique_ptr<IPackageWriteOperation>
		{ return (Writer ? Writer : GetFilePackageWriter())->Begin(std::move(Files)); }
	};
	COREDOBJECT_API auto ToPackageSaveResult(FPackageWriteResult Result) -> FPackageSaveResult;
	using FPackageDestinationResolver = std::function<std::filesystem::path(const DPackage&)>;
	COREDOBJECT_API auto SetPackageDestinationResolver(FPackageDestinationResolver Resolver) -> void;

	// Package persistence only: no asset admission, catalog publication or recovery policy.
	// Own and destroy on GameThread before object-system/scheduler shutdown.
	class COREDOBJECT_API FPackageSaveOperation
	{
	public:
		~FPackageSaveOperation();
		static auto Begin(DPackage* Package, const FPackageSaveOptions& Options,
			FPackageSaveResult& Admission, bool bAsync = true) -> std::unique_ptr<FPackageSaveOperation>;
		static auto Begin(DPackage* Package, const FSavePackageContext& Context,
			FPackageSaveResult& Admission, bool bAsync = true, bool bDeferStaging = false) -> std::unique_ptr<FPackageSaveOperation>;
		auto StageDetached() -> void;
		auto IsStagingReady() const -> bool;
		auto IsCompleted() const -> bool;
		auto Complete() -> FPackageSaveResult;
		// Waits only for I/O; never waits on queued GameThread work.
		auto WaitAndComplete() -> FPackageSaveResult;
		auto Cancel() -> FPackageSaveResult;
		auto GetDetachedBytes() const -> uint64;
		// Transaction coordinator seam: retain rollback state until FinalizeCommit.
		auto CommitStaged() -> FPackageSaveResult;
		auto RollbackCommit() -> FPackageSaveResult;
		auto FinalizeCommit() -> FPackageSaveResult;
	private:
		FPackageSaveOperation();
		struct FState;
		std::unique_ptr<FState> State;
	};
}
