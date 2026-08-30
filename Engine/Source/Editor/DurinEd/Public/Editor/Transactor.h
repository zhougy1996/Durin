#pragma once

#include "DurinEdAPI.h"
#include "DObject/Object.h"
#include "Editor/Transaction.h"
#include "Editor/TransactionObjectRecord.h"
#include "Editor/TransactionRecord.h"

#include "Transactor.gen.h"

namespace Durin::Editor
{
	using FTransactionScopeId = uint64;

	// Describes one producing tool and the optional primary participant for a transaction.
	struct FTransactionContext
	{
		std::string Name;
		std::string Description;
		FPersistentObjectRef PrimaryObject;
	};

	// Bounds retained native allocations; managed graphs reached through records are excluded.
	struct FTransactionBufferLimits
	{
		size_t MaximumEntries = 256;
		size_t MaximumOwnedBytes = 64u * 1024u * 1024u;

		friend auto operator==(const FTransactionBufferLimits&, const FTransactionBufferLimits&)
			-> bool = default;
	};

	// Identifies the current non-reentrant operation performed by a transactor.
	enum class ETransactorState : uint8
	{
		Idle,
		Recording,
		Executing,
		Undoing,
		Redoing,
		Destroying,
	};

	// Distinguishes successful, no-op, discarded, and failed synchronous outcomes.
	enum class ETransactorResultCode : uint8
	{
		Succeeded,
		NoOp,
		Discarded,
		Rejected,
		Failed,
	};

	struct [[nodiscard]] FTransactorResult
	{
		ETransactorResultCode Code = ETransactorResultCode::Rejected;
		FTransactionId TransactionId = 0;
		FTransactionScopeId ScopeId = 0;
		uint64 RecordId = 0;
		std::string Message;

		auto IsSuccess() const -> bool { return Code == ETransactorResultCode::Succeeded; }
		explicit operator bool() const { return IsSuccess(); }
	};

	struct FTransactionPackageRevisionTransition
	{
		FPersistentObjectRef Package;
		FRevisionId BeforeRevision = 0;
		FRevisionId AfterRevision = 0;
		FRevisionId InitialSavedRevision = 0;
		bool bInitialCheckpointValid = false;
	};

	// Owns one executable property record or domain-specific custom change.
	class FTransactionRecord
	{
	public:
		explicit FTransactionRecord(FTransactionObjectRecord Record)
			: Data(std::move(Record)) {}
		explicit FTransactionRecord(std::unique_ptr<ITransactionCustomChange> Change)
			: Data(std::move(Change)) {}

		DURINED_API auto Validate(std::string* OutError = nullptr) const -> bool;
		auto IsNoOp() const -> bool;
		DURINED_API auto Apply(
			bool bBefore,
			EPropertyChangeOrigin Origin,
			std::string* OutError = nullptr) -> bool;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto IsDeferredOperationPending() const -> bool;
		DURINED_API auto SetDeferredOperationCompletion(
			FTransactionDeferredCompletion Completion) -> void;
		DURINED_API auto GetDetails(ETransactionOperation Operation) const -> std::string;
		DURINED_API auto GetAffectedPackages() const -> std::span<DPackage* const>;
		DURINED_API auto MutatesMountedContent() const -> bool;
		DURINED_API auto IsOwnedByModule(std::string_view ModuleName) const -> bool;
		DURINED_API auto GetObjectTarget() const -> DObject*;

	private:
		std::variant<FTransactionObjectRecord,
			std::unique_ptr<ITransactionCustomChange>> Data;
	};

	// Owns one immutable candidate or retained transaction and all of its record data.
	class FTransaction
	{
	public:
		DURINED_API FTransaction(FTransactionId Id, FTransactionContext Context);
		FTransaction(const FTransaction&) = delete;
		auto operator=(const FTransaction&) -> FTransaction& = delete;
		FTransaction(FTransaction&&) noexcept = default;
		auto operator=(FTransaction&&) noexcept -> FTransaction& = default;

		auto GetId() const -> FTransactionId { return Id; }
		auto GetContext() const -> const FTransactionContext& { return Context; }
		auto GetRecordCount() const -> size_t { return Records.size(); }
		DURINED_API auto AddRecord(FTransactionObjectRecord Record) -> uint64;
		DURINED_API auto AddRecord(std::unique_ptr<ITransactionCustomChange> Change) -> uint64;
		DURINED_API auto UpdateRecord(uint64 RecordId, FTransactionObjectRecord Record) -> bool;
		DURINED_API auto TruncateRecords(size_t Count) -> void;
		DURINED_API auto RemoveNoOpRecords() -> void;
		DURINED_API auto Validate(std::string* OutError = nullptr) const -> bool;
		DURINED_API auto Apply(
			bool bUndo,
			EPropertyChangeOrigin Origin,
			std::string* OutError = nullptr) -> bool;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetOwnedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto IsDeferredOperationPending() const -> bool;
		DURINED_API auto SetDeferredOperationCompletion(
			FTransactionDeferredCompletion Completion) -> void;
		DURINED_API auto GetDetails(ETransactionOperation Operation) const -> std::string;
		DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
		DURINED_API auto MutatesMountedContent() const -> bool;
		DURINED_API auto IsOwnedByModule(std::string_view ModuleName) const -> bool;
		auto GetPackageTransitions() const
			-> std::span<const FTransactionPackageRevisionTransition>
		{
			return PackageTransitions;
		}
		auto HasPackageTransitions() const -> bool { return !PackageTransitions.empty(); }
		DURINED_API auto SetPackageTransitions(
			std::vector<FTransactionPackageRevisionTransition> Transitions) -> void;
		DURINED_API auto ReferencesPackage(const DPackage& Package) const -> bool;

	private:
		FTransactionId Id = 0;
		FTransactionContext Context;
		std::vector<FTransactionRecord> Records;
		std::vector<FTransactionPackageRevisionTransition> PackageTransitions;
	};
}

namespace Durin
{
	struct FTransBufferTestAccess;

	// Defines the reflected editor recording and Undo/Redo service boundary.
	DCLASS(Abstract)
	class DTransactor : public DObject
	{
		GENERATED_BODY()

	public:
		DURINED_API virtual auto Begin(const Editor::FTransactionContext& Context)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto Record(
			Editor::FTransactionScopeId ScopeId,
			Editor::FTransactionObjectRecord Record)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto Execute(
			std::unique_ptr<Editor::ITransactionCustomChange> Change,
			bool bAlreadyApplied = false) -> Editor::FTransactorResult;
		[[nodiscard]] DURINED_API virtual auto CommitApplied(
			std::unique_ptr<Editor::ITransactionCustomChange> Change)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto UpdateRecord(
			Editor::FTransactionScopeId ScopeId,
			uint64 RecordId,
			Editor::FTransactionObjectRecord Record) -> Editor::FTransactorResult;
		DURINED_API virtual auto End(Editor::FTransactionScopeId ScopeId)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto Cancel(Editor::FTransactionScopeId ScopeId)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto Undo() -> Editor::FTransactorResult;
		DURINED_API virtual auto Undo(Editor::FTransactionId ExpectedId)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto Redo() -> Editor::FTransactorResult;
		DURINED_API virtual auto Redo(Editor::FTransactionId ExpectedId)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto Reset() -> Editor::FTransactorResult;
		DURINED_API virtual auto RemoveTransaction(Editor::FTransactionId TransactionId)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto SetTransactionCompletion(
			Editor::FTransactionId TransactionId,
			Editor::FTransactionDeferredCompletion Completion) -> Editor::FTransactorResult;
		DURINED_API virtual auto IsTransactionPending(
			Editor::FTransactionId TransactionId) const -> bool;
		DURINED_API virtual auto GetTransactionDetails(
			Editor::FTransactionId TransactionId,
			Editor::ETransactionOperation Operation) const -> std::string;
		DURINED_API virtual auto DiscardCustomChangesByModule(std::string_view ModuleName)
			-> Editor::FTransactorResult;
		DURINED_API virtual auto CanUndo() const -> bool;
		DURINED_API virtual auto CanRedo() const -> bool;
		DURINED_API virtual auto HasPendingOperation() const -> bool;
		DURINED_API virtual auto GetUndoId() const -> Editor::FTransactionId;
		DURINED_API virtual auto GetRedoId() const -> Editor::FTransactionId;
		DURINED_API virtual auto GetUndoDescription() const -> std::string_view;
		DURINED_API virtual auto GetRedoDescription() const -> std::string_view;
		DURINED_API virtual auto ConsumeEvents()
			-> std::vector<Editor::FTransactionEvent>;
		DURINED_API virtual auto GetMountedContentMutationRevision() const -> uint64;
		DURINED_API virtual auto NotifyMountedContentMutation() -> void;
		DURINED_API virtual auto EstablishSavedState(DPackage& Package) -> void;
		DURINED_API virtual auto MarkSaved(DPackage& Package) -> void;
		DURINED_API virtual auto InvalidateSavedState(DPackage& Package) -> void;
		DURINED_API virtual auto GetPackageRevisionState(const DPackage& Package) const
			-> std::optional<Editor::FPackageRevisionState>;
		DURINED_API virtual auto ForgetPackage(DPackage& Package) -> void;

	protected:
		DURINED_API explicit DTransactor(const FObjectInitializer& ObjectInitializer);
	};

	// Owns bounded transaction data, structural history position, and GC-visible references.
	DCLASS()
	class DTransBuffer final : public DTransactor
	{
		GENERATED_BODY()

	public:
		DURINED_API explicit DTransBuffer(const FObjectInitializer& ObjectInitializer);
		DURINED_API auto Begin(const Editor::FTransactionContext& Context)
			-> Editor::FTransactorResult override;
		DURINED_API auto Record(
			Editor::FTransactionScopeId ScopeId,
			Editor::FTransactionObjectRecord Record)
			-> Editor::FTransactorResult override;
		DURINED_API auto Execute(
			std::unique_ptr<Editor::ITransactionCustomChange> Change,
			bool bAlreadyApplied = false) -> Editor::FTransactorResult override;
		DURINED_API auto UpdateRecord(
			Editor::FTransactionScopeId ScopeId,
			uint64 RecordId,
			Editor::FTransactionObjectRecord Record) -> Editor::FTransactorResult override;
		DURINED_API auto End(Editor::FTransactionScopeId ScopeId)
			-> Editor::FTransactorResult override;
		DURINED_API auto Cancel(Editor::FTransactionScopeId ScopeId)
			-> Editor::FTransactorResult override;
		DURINED_API auto Undo() -> Editor::FTransactorResult override;
		DURINED_API auto Undo(Editor::FTransactionId ExpectedId)
			-> Editor::FTransactorResult override;
		DURINED_API auto Redo() -> Editor::FTransactorResult override;
		DURINED_API auto Redo(Editor::FTransactionId ExpectedId)
			-> Editor::FTransactorResult override;
		DURINED_API auto Reset() -> Editor::FTransactorResult override;
		DURINED_API auto RemoveTransaction(Editor::FTransactionId TransactionId)
			-> Editor::FTransactorResult override;
		DURINED_API auto SetTransactionCompletion(
			Editor::FTransactionId TransactionId,
			Editor::FTransactionDeferredCompletion Completion) -> Editor::FTransactorResult override;
		DURINED_API auto IsTransactionPending(
			Editor::FTransactionId TransactionId) const -> bool override;
		DURINED_API auto GetTransactionDetails(
			Editor::FTransactionId TransactionId,
			Editor::ETransactionOperation Operation) const -> std::string override;
		DURINED_API auto DiscardCustomChangesByModule(std::string_view ModuleName)
			-> Editor::FTransactorResult override;
		DURINED_API auto SetLimits(Editor::FTransactionBufferLimits Limits)
			-> Editor::FTransactorResult;
		DURINED_API auto CanUndo() const -> bool override;
		DURINED_API auto CanRedo() const -> bool override;
		DURINED_API auto HasPendingOperation() const -> bool override;
		DURINED_API auto ConsumeEvents() -> std::vector<Editor::FTransactionEvent> override;
		DURINED_API auto GetMountedContentMutationRevision() const -> uint64 override;
		DURINED_API auto NotifyMountedContentMutation() -> void override;
		DURINED_API auto EstablishSavedState(DPackage& Package) -> void override;
		DURINED_API auto MarkSaved(DPackage& Package) -> void override;
		DURINED_API auto InvalidateSavedState(DPackage& Package) -> void override;
		DURINED_API auto GetPackageRevisionState(const DPackage& Package) const
			-> std::optional<Editor::FPackageRevisionState> override;
		DURINED_API auto ForgetPackage(DPackage& Package) -> void override;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		DURINED_API auto BeginDestroy() -> void override;

		auto GetState() const -> Editor::ETransactorState { return State; }
		auto GetLimits() const -> Editor::FTransactionBufferLimits { return Limits; }
		auto GetHistoryCount() const -> size_t { return History.size(); }
		auto GetUndoCount() const -> size_t { return Cursor; }
		auto GetRedoCount() const -> size_t { return History.size() - Cursor; }
		auto GetOwnedBytes() const -> size_t { return OwnedBytes; }
		DURINED_API auto GetUndoId() const -> Editor::FTransactionId override;
		DURINED_API auto GetRedoId() const -> Editor::FTransactionId override;
		DURINED_API auto GetUndoDescription() const -> std::string_view override;
		DURINED_API auto GetRedoDescription() const -> std::string_view override;

	private:
		struct FSavepoint
		{
			Editor::FTransactionScopeId ScopeId = 0;
			size_t RecordCount = 0;
			size_t OwnedBytes = 0;
		};

		struct FTrackedPackageState
		{
			Editor::FPersistentObjectRef Package;
			Editor::FRevisionId CurrentRevision = 0;
			Editor::FRevisionId SavedRevision = 0;
			bool bCheckpointValid = false;
		};

		auto CheckThread() const -> void;
		auto Reject(std::string Message) const -> Editor::FTransactorResult;
		auto CloseScope(Editor::FTransactionScopeId ScopeId, bool bCancel)
			-> Editor::FTransactorResult;
		auto FinalizePending() -> Editor::FTransactorResult;
		auto CompleteDeferredOperation(
			Editor::ETransactionOperation Operation,
			Editor::FTransactionId TransactionId,
			bool bSucceeded) -> void;
		auto FindTransaction(Editor::FTransactionId TransactionId)
			-> Editor::FTransaction*;
		auto FindTransaction(Editor::FTransactionId TransactionId) const
			-> const Editor::FTransaction*;
		auto RecalculateOwnedBytes() -> bool;
		auto EnforceLimits() -> void;
		auto QueueEvent(Editor::ETransactionEventType Type,
			const Editor::FTransaction& Transaction,
			std::string Details = {}) -> void;
		auto AllocateRevision() -> Editor::FRevisionId;
		auto PreparePackageTransitions(Editor::FTransaction& Transaction) -> void;
		auto ApplyPackageTransitions(const Editor::FTransaction& Transaction,
			bool bForward) -> void;
		auto FindPackageState(const DPackage& Package) -> FTrackedPackageState*;
		auto FindPackageState(const DPackage& Package) const -> const FTrackedPackageState*;
		auto EnsurePackageState(DPackage& Package) -> FTrackedPackageState&;
		auto SynchronizeDirtyState(FTrackedPackageState& State) -> void;

		Editor::FTransactionBufferLimits Limits;
		Editor::ETransactorState State = Editor::ETransactorState::Idle;
		Editor::FTransactionId NextTransactionId = 1;
		Editor::FTransactionScopeId NextScopeId = 1;
		std::optional<Editor::FTransaction> Pending;
		std::vector<FSavepoint> Savepoints;
		std::vector<Editor::FTransaction> History;
		size_t Cursor = 0;
		size_t OwnedBytes = 0;
		std::vector<Editor::FTransactionEvent> Events;
		Editor::FTransactionId PendingTransactionId = 0;
		Editor::ETransactionOperation PendingOperation =
			Editor::ETransactionOperation::Execute;
		Editor::FTransactionDeferredCompletion TransactionCompletion;
		Editor::FRevisionId NextRevision = 1;
		uint64 MountedContentMutationRevision = 1;
		std::unordered_map<DPackage*, FTrackedPackageState> PackageStates;

		friend struct FTransBufferTestAccess;
	};
}

namespace Durin::Editor
{
	// Move-only RAII boundary with automatic reflected-object before/after recording.
	class FScopedTransaction
	{
	public:
		// Uses the editor's active transactor. An absent editor leaves the scope inactive.
		DURINED_API explicit FScopedTransaction(std::string_view Description);
		DURINED_API FScopedTransaction(DTransactor* Transactor, FTransactionContext Context);
		DURINED_API ~FScopedTransaction();
		FScopedTransaction(const FScopedTransaction&) = delete;
		auto operator=(const FScopedTransaction&) -> FScopedTransaction& = delete;
		DURINED_API FScopedTransaction(FScopedTransaction&& Other) noexcept;
		DURINED_API auto operator=(FScopedTransaction&& Other) noexcept
			-> FScopedTransaction&;
		// Best-effort captures reflected members before mutation and their final values
		// at scope end. Repeated calls are idempotent, and recording failure does not
		// prevent the caller's mutation.
		DURINED_API auto Modify(DObject* Object) -> void;
		auto Modify(DObject& Object) -> void { Modify(&Object); }
		// Advanced property-editing entry points keep records bound to this exact scope.
		[[nodiscard]] DURINED_API auto Record(FTransactionObjectRecord Record)
			-> FTransactorResult;
		[[nodiscard]] DURINED_API auto UpdateRecord(
			uint64 RecordId,
			FTransactionObjectRecord Record) -> FTransactorResult;
		DURINED_API auto End() -> FTransactorResult;
		DURINED_API auto Cancel() -> FTransactorResult;
		auto IsActive() const -> bool { return Transactor != nullptr && ScopeId != 0; }

	private:
		struct FModifiedProperty;
		auto CaptureModifiedObject(DObject* Object) -> FTransactorResult;
		auto PrepareModifiedRecords() -> FTransactorResult;

		DTransactor* Transactor = nullptr;
		FTransactionScopeId ScopeId = 0;
		std::vector<std::unique_ptr<FModifiedProperty>> ModifiedProperties;
	};
}
