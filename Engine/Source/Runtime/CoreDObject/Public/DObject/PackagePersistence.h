#pragma once

#include "CoreMinimal.h"
#include "DObject/PackageCapture.h"
#include "Misc/PackageWriter.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace PackageSaveErrors
	{
		inline constexpr UnifiedError::FErrorId InvalidPath{"PackageSave", "InvalidPath"};
		inline constexpr UnifiedError::FErrorId InvalidPackageType{"PackageSave", "InvalidPackageType"};
		inline constexpr UnifiedError::FErrorId IoError{"PackageSave", "IoError"};
		inline constexpr UnifiedError::FErrorId StaleData{"PackageSave", "StaleData"};
		inline constexpr UnifiedError::FErrorId Cancelled{"PackageSave", "Cancelled"};
		inline constexpr UnifiedError::FErrorId Busy{"PackageSave", "Busy"};
		inline constexpr UnifiedError::FErrorId ShuttingDown{"PackageSave", "ShuttingDown"};
	}

	namespace PackageSavePrivate
	{
		// Internal detached-work owner shared by file-only and Engine adapters.
		COREDOBJECT_API auto CheckAsyncAdmission(uint64 Bytes = 0) -> std::expected<void, UnifiedError::FError>;
		struct FAsyncSaveState;
		using FAsyncSavePublicationFactory = std::function<Tasks::FTask(const Tasks::FTask&,
			const std::shared_ptr<FAsyncSaveState>&)>;
		COREDOBJECT_API auto SubmitAsyncSaveTask(uint64 Bytes, std::function<void()> Write,
			FAsyncSavePublicationFactory MakePublication, std::function<void()> Cleanup) -> std::expected<void, UnifiedError::FError>;
		COREDOBJECT_API auto BeginAsyncSavePublication(const std::shared_ptr<FAsyncSaveState>& Save) -> void;
		COREDOBJECT_API auto EndAsyncSavePublication() -> void;
		// Keep module-local logging out of this shared-header template.
		COREDOBJECT_API auto ReportAsyncSaveAbandonmentException() -> void;

		// Domain task factory: the result is returned by an ordinary publication body.
		// Abandon handles a body that throws or never runs; it never supplies a result.
		// Both callbacks release their object captures on GameThread before notification
		// when the body runs, or after producer retirement when the owner reaps it.
		template<typename TResult, typename F>
		auto SubmitTypedAsyncSave(uint64 Bytes, std::function<void()> Write, F&& Publish,
			std::function<void()> Abandon) -> std::expected<Tasks::TTask<TResult>, UnifiedError::FError>
		{
			struct FCallbacks
			{
				std::optional<std::decay_t<F>> Publish;
				std::function<void()> Abandon;
			};
			auto Callbacks = std::make_shared<FCallbacks>(FCallbacks{std::forward<F>(Publish), std::move(Abandon)});
			Tasks::TTask<TResult> Task;
			auto Admission = SubmitAsyncSaveTask(Bytes, std::move(Write),
				[&Task, Callbacks](const Tasks::FTask& Disk, const std::shared_ptr<FAsyncSaveState>& Save) {
					Task = Tasks::LaunchGameThreadDeferred("Package.SavePublication",
						[Disk, Save, Callbacks]() mutable -> TResult {
							BeginAsyncSavePublication(Save);
							struct FScope { ~FScope() { EndAsyncSavePublication(); } } Scope;
							auto OnAbandon = std::exchange(Callbacks->Abandon, {});
							try {
								auto Invoke = std::move(*Callbacks->Publish);
								Callbacks->Publish.reset();
								return std::invoke(Invoke, Disk.GetState() == ETaskState::Succeeded);
							}
							catch (...) {
								Callbacks->Publish.reset();
								try { if (OnAbandon) OnAbandon(); }
								catch (...) { ReportAsyncSaveAbandonmentException(); }
								throw;
							}
						}, Disk);
					return Tasks::FTask(Task);
				},
				[Callbacks] {
					Callbacks->Publish.reset();
					auto OnAbandon = std::exchange(Callbacks->Abandon, {});
					if (OnAbandon) OnAbandon();
				});
			if (!Admission) return std::unexpected(std::move(Admission.error()));
			return Task;
		}
		COREDOBJECT_API auto PollAsyncSaves() -> void;
		COREDOBJECT_API auto SetAsyncSaveAdmission(bool bAccepting) -> void;
		enum class EPublicationFault : uint8 { None, RejectAdmission };
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
		std::function<bool()> IsCancellationRequested;
	};
	struct FSavePackageContext
	{
		FPackageSaveOptions Options;
		std::shared_ptr<IPackageWriter> Writer = GetFilePackageWriter();
		COREDOBJECT_API auto SaveAsync(DPackage* Package) const
			-> Tasks::TTask<FPackageWriteResult>;
		auto Capture(DPackage* Package, ObjectPackage::FLinkerTables& OutLinker,
			uint32 FormatVersion = ObjectPackage::DastV12FormatVersion, std::vector<FObjectKey>* OutExports = nullptr) const
			-> std::expected<void, FPackageCaptureError>
		{
			return CapturePackageLinker(Package, Options.Mode == EPackageSaveMode::Complete
				? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, Options.Capture, OutLinker, FormatVersion, OutExports);
		}
		// Detached callers supply file order and recovery paths; ownership transfers.
		auto BeginWrite(std::vector<FPackageWriteFile> Files) const
			-> std::unique_ptr<IPackageWriteOperation>
		{ return (Writer ? Writer : GetFilePackageWriter())->Begin(std::move(Files)); }
	};
	using FPackageDestinationResolver = std::function<std::filesystem::path(const DPackage&)>;
	COREDOBJECT_API auto SetPackageDestinationResolver(FPackageDestinationResolver Resolver) -> void;

	// Package persistence only: no asset admission, catalog publication or recovery policy.
	// Own and destroy on GameThread before object-system/scheduler shutdown.
	class COREDOBJECT_API FPackageSaveOperation
	{
	public:
		~FPackageSaveOperation();
		static auto Begin(DPackage* Package, const FPackageSaveOptions& Options,
			std::optional<UnifiedError::FError>* OutError = nullptr, bool bAsync = true) -> std::unique_ptr<FPackageSaveOperation>;
		static auto Begin(DPackage* Package, const FSavePackageContext& Context,
			std::optional<UnifiedError::FError>* OutError = nullptr, bool bAsync = true, bool bDeferStaging = false) -> std::unique_ptr<FPackageSaveOperation>;
		auto StageDetached() -> void;
		auto IsStagingReady() const -> bool;
		auto IsCompleted() const -> bool;
		auto Complete() -> FPackageWriteResult;
		// Waits only for I/O; never waits on queued GameThread work.
		auto WaitAndComplete() -> FPackageWriteResult;
		auto Cancel() -> FPackageWriteResult;
		auto GetDetachedBytes() const -> uint64;
		// Transaction coordinator seam: retain rollback state until FinalizeCommit.
		auto CommitStaged() -> FPackageWriteResult;
		auto RollbackCommit() -> FPackageWriteResult;
		auto FinalizeCommit() -> FPackageWriteResult;
	private:
		FPackageSaveOperation();
		struct FState;
		std::unique_ptr<FState> State;
	};
}
