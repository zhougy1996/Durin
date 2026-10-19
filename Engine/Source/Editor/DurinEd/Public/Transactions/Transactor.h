#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "AssetRegistry/ContentChanges.h"
#include "DObject/Object.h"
#include "Misc/ITransaction.h"
#include "DObject/StrongObjectPtr.h"
#include "Transactions/Transaction.h"
#include "Transactions/TransactionObjectRecord.h"
#include "Transactions/TransactionRecord.h"
#include "Asset/PackageReload.h"

#include "Transactor.gen.h"

namespace Durin { class DTransactor; class DTransBuffer; }

namespace Durin::Editor
{

	class FPropertyEditSession;
	class FScopedTransaction;

	// Describes one producing tool and the optional primary participant for a transaction.
	struct FTransactionContext
	{
		std::string Name;
		std::string Description;
		FPersistentObjectRef PrimaryObject;
	};

	// Identifies the current non-reentrant operation performed by a transactor.
	enum class ETransactorState : uint8
	{
		Idle,
		Recording,
		Undoing,
		Redoing,
		Destroying,
		RecoveryRequired,
	};

	// Distinguishes successful, no-op, discarded, and failed synchronous outcomes.
	enum class ETransactorResultCode : uint8
	{
		Succeeded,
		NoOp,
		Discarded,
		Rejected,
		Failed,
		RecoveryRequired,
	};

	enum class ETransactionApplyFailure : uint8
	{
		ValidationFailed,
		Restored,
		RecoveryRequired
	};

	using FTransactionRecordResult = std::expected<void, std::string>;

	struct FTransactionRollbackFailure
	{
		size_t RecordIndex = 0;
		std::string Message;
	};

	// Retains execution disposition and messages without exposing record-specific errors.
	struct FTransactionApplyError
	{
		ETransactionApplyFailure Status = ETransactionApplyFailure::ValidationFailed;
		size_t RecordIndex = 0;
		std::string Message;
		std::vector<FTransactionRollbackFailure> RollbackFailures;
	};

	using FTransactionApplyResult = std::expected<void, FTransactionApplyError>;

	enum class ETransactorRejectionReason : uint8
	{
		BeginState, RecordScope, ExecuteState, MissingCustom,
		UpdateScope, MissingRecord, UndoState,
		UndoHead, RedoState, RedoHead, ResetState,
		ModuleName, ModuleState
	};
	struct FTransactorRejection
	{
		ETransactorRejectionReason Reason = ETransactorRejectionReason::BeginState;
		ETransactorState State = ETransactorState::Idle;
		uint64 RequestedId = 0;
		FTransactionId PendingId = 0;

		FTransactionId UndoHead = 0;
		FTransactionId RedoHead = 0;
		size_t HistoryCount = 0;
		size_t OwnedBytes = 0;
		std::string ModuleName;
	};
	DURINED_API auto FormatTransactorRejection(const FTransactorRejection& Rejection) -> std::string;

	enum class ETransactorFailure : uint8 { EntryAccounting, RetainedAccounting, ModuleAccounting };
	struct FTransactorFailure
	{
		ETransactorFailure Code = ETransactorFailure::EntryAccounting;
		std::string Module;
		size_t Actual = 0;
	};

	enum class ETransactorNotice : uint8 { None, NothingUndo, NothingRedo, ModuleDrain };

	struct [[nodiscard]] FTransactorResult
	{
		ETransactorResultCode Code = ETransactorResultCode::Rejected;
		FTransactionId TransactionId = 0;

		ETransactorNotice Notice = ETransactorNotice::None;
		size_t NoticeCount = 0;
		std::shared_ptr<const FTransactionApplyError> ApplyCause;
		std::optional<FTransactorRejection> RejectionCause;
		std::optional<FTransactorFailure> FailureCause;

		auto IsSuccess() const -> bool { return Code == ETransactorResultCode::Succeeded; }
		explicit operator bool() const { return IsSuccess(); }
	};

	DURINED_API auto ResolveEditorTransactor(ETransactionMode Mode) -> std::expected<DTransactor*, std::string>;

	DURINED_API auto FormatTransactorResult(const FTransactorResult& Result) -> std::string;
	DURINED_API auto ApplyEditorChange(DTransactor& Transactor, std::unique_ptr<FCommandChange> Change) -> FTransactorResult;

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
		explicit FTransactionRecord(std::unique_ptr<FCommandChange> Change, DObject* Object = nullptr)
			: Data(std::move(Change)), CustomObject(Object) {}

		DURINED_API auto Validate() const -> FTransactionRecordResult;
		DURINED_API auto CaptureAfter() -> FTransactionRecordResult;
		auto IsNoOp() const -> bool;
		DURINED_API auto Apply(
			bool bBefore,
			EPropertyChangeOrigin Origin, bool* OutRecoveryRequired = nullptr) -> FTransactionRecordResult;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto GetDetails(ETransactionOperation Operation) const -> std::string;
		DURINED_API auto GetAffectedPackages() const -> std::span<DPackage* const>;
		DURINED_API auto MutatesMountedContent() const -> bool;
		DURINED_API auto IsOwnedByModule(std::string_view ModuleName) const -> bool;
		DURINED_API auto GetObjectTarget() const -> DObject*;

	private:
		friend class FTransaction;
		std::variant<FTransactionObjectRecord,
			std::unique_ptr<FCommandChange>> Data;
		FPersistentObjectRef CustomObject;
	};

	// Owns one immutable candidate or retained transaction and all of its record data.
	class FTransaction : public ITransaction
	{
		friend class ::Durin::DTransBuffer;
	public:
		DURINED_API FTransaction(FTransactionId Id, FTransactionContext Context, DTransBuffer* RecordingOwner = nullptr);
		FTransaction(const FTransaction&) = delete;
		auto operator=(const FTransaction&) -> FTransaction& = delete;
		FTransaction(FTransaction&&) noexcept = default;
		auto operator=(FTransaction&&) noexcept -> FTransaction& = default;

		auto GetId() const -> FTransactionId { return Id; }
		auto GetContext() const -> const FTransactionContext& { return Context; }
		auto GetRecordCount() const -> size_t { return Records.size(); }
		DURINED_API auto AddRecord(FTransactionObjectRecord Record) -> uint64;
		DURINED_API auto AddRecord(std::unique_ptr<FCommandChange> Change, DObject* Object = nullptr) -> uint64;
		DURINED_API auto UpdateRecord(uint64 RecordId, FTransactionObjectRecord Record) -> bool;
		DURINED_API auto RemoveNoOpRecords() -> void;
		DURINED_API auto SaveObject(DObject* Object) -> void override;
		DURINED_API auto ContainsObject(const DObject* Object) const -> bool override;
		DURINED_API auto FinalizeObjects() -> FTransactionRecordResult;
		DURINED_API auto Validate() const -> FTransactionApplyResult;
		DURINED_API auto Apply(
			bool bUndo,
			EPropertyChangeOrigin Origin
		) -> FTransactionApplyResult;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetOwnedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto GetDetails(ETransactionOperation Operation) const -> std::string;
		DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
		DURINED_API auto GetTransactionObjects() const -> std::vector<DObject*>;
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
		// Includes modified packages and collector-enumerated hard lifetime dependencies.
		DURINED_API auto ReferencesPackage(const DPackage& Package) const -> bool;

	private:
		DURINED_API auto CaptureObject(DObject* Object) -> FTransactionRecordResult;
		DTransBuffer* RecordingOwner = nullptr;
		FTransactionId Id = 0;
		FTransactionContext Context;
		std::vector<FTransactionRecord> Records;
		std::vector<FPersistentObjectRef> CapturedObjects;
		std::vector<uint64> CapturedRecordIds;
		std::vector<FTransactionPackageRevisionTransition> PackageTransitions;
	};
}

namespace Durin
{
	struct FTransBufferTestAccess;
	class FTransactorReloadParticipant;

	// Requires a complete editor recording, history and package-revision implementation.
	DCLASS(Abstract)
	class DTransactor : public DObject
	{
		GENERATED_BODY()

	public:
		DURINED_API virtual auto Begin(std::string_view Context, std::string_view Description) -> int32 = 0;
		DURINED_API virtual auto End() -> int32 = 0;
		DURINED_API virtual auto Cancel(int32 StartIndex = 0) -> void = 0;
		DURINED_API virtual auto IsActive() const -> bool = 0;
		DURINED_API virtual auto StoreUndo(DObject* Object, std::unique_ptr<Editor::FCommandChange> Change) -> Editor::FTransactorResult = 0;
		auto StoreUndo(std::unique_ptr<Editor::FCommandChange> Change) -> Editor::FTransactorResult
		{ return StoreUndo(nullptr, std::move(Change)); }
		DURINED_API virtual auto Undo(bool bCanRedo = true) -> bool = 0;
		DURINED_API virtual auto UndoExpected(Editor::FTransactionId ExpectedId) -> Editor::FTransactorResult = 0;
		DURINED_API virtual auto Redo() -> bool = 0;
		DURINED_API virtual auto RedoExpected(Editor::FTransactionId ExpectedId) -> Editor::FTransactorResult = 0;
		DURINED_API virtual auto Reset(std::string_view Reason = {}) -> Editor::FTransactorResult = 0;
		DURINED_API virtual auto CanTransact() const -> bool = 0;
		DURINED_API virtual auto SetPrimaryUndoObject(DObject* Object) -> void = 0;
		auto GetLastResult() const -> const Editor::FTransactorResult& { return LastResult; }
		DURINED_API virtual auto GetTransactionDetails(
			Editor::FTransactionId TransactionId,
			Editor::ETransactionOperation Operation) const -> std::string = 0;
		DURINED_API virtual auto DiscardCustomChangesByModule(std::string_view ModuleName)
			-> Editor::FTransactorResult = 0;
		DURINED_API virtual auto CanUndo(std::string* Reason = nullptr) const -> bool = 0;
		DURINED_API virtual auto CanRedo(std::string* Reason = nullptr) const -> bool = 0;
		DURINED_API virtual auto GetUndoId() const -> Editor::FTransactionId = 0;
		DURINED_API virtual auto GetRedoId() const -> Editor::FTransactionId = 0;
		using FTransactionListener = std::function<void(const Editor::FTransactionEvent&)>;
		DURINED_API auto AddTransactionListener(FTransactionListener Listener) -> uint64;
		DURINED_API auto RemoveTransactionListener(uint64 Handle) -> void;
		DURINED_API virtual auto DispatchEvents() -> void = 0;
		DURINED_API virtual auto GetQueueLength() const -> size_t = 0;
		DURINED_API virtual auto GetTransaction(size_t Index) const -> const Editor::FTransaction* = 0;
		DURINED_API virtual auto GetUndoContext(bool bCheckWhetherUndoPossible = true) const -> Editor::FTransactionContext = 0;
		DURINED_API virtual auto GetRedoContext() const -> Editor::FTransactionContext = 0;
		virtual auto GetUndoCount() const -> size_t = 0;
		DURINED_API virtual auto GetMountedContentMutationRevision() const -> uint64 = 0;
		DURINED_API virtual auto NotifyMountedContentMutation() -> void = 0;
		DURINED_API virtual auto NotifyMountedContentMutation(FContentChangeBatch Changes) -> void = 0;
		DURINED_API virtual auto CaptureMountedContentChanges(uint64 FromRevision) const -> FContentChangeBatch = 0;
		DURINED_API virtual auto EstablishSavedState(DPackage& Package) -> void = 0;
		DURINED_API virtual auto MarkSaved(DPackage& Package) -> void = 0;
		DURINED_API virtual auto InvalidateSavedState(DPackage& Package) -> void = 0;
		DURINED_API virtual auto GetPackageRevisionState(const DPackage& Package) const
			-> std::optional<Editor::FPackageRevisionState> = 0;
		DURINED_API virtual auto ForgetPackage(DPackage& Package) -> void = 0;

	protected:
		DURINED_API explicit DTransactor(const FObjectInitializer& ObjectInitializer);
		Editor::FTransactorResult LastResult;
		std::map<uint64, FTransactionListener> Listeners;
		uint64 NextListener = 1;
	private:
		friend class Editor::FScopedTransaction;
		// Exact scope validity; zero means no supported live recording.
		virtual auto GetRecordingId() const -> Editor::FTransactionId = 0;
		friend auto Editor::ApplyEditorChange(DTransactor&, std::unique_ptr<Editor::FCommandChange>) -> Editor::FTransactorResult;
		DURINED_API virtual auto ReportChangeFailure(std::unique_ptr<Editor::FCommandChange> Change, Editor::FCommandChangeError Error) -> Editor::FTransactorResult = 0;
		friend class Editor::FPropertyEditSession;
		friend struct FTransBufferTestAccess;
		DURINED_API virtual auto Record(Editor::FTransactionObjectRecord Record) -> std::expected<uint64, Editor::FTransactorResult> = 0;
		DURINED_API virtual auto UpdateRecord(uint64 RecordId, Editor::FTransactionObjectRecord Record) -> Editor::FTransactorResult = 0;
	};

	// Owns bounded transaction data, structural history position, and GC-visible references.
	DCLASS()
	class DTransBuffer final : public DTransactor
	{
		GENERATED_BODY()

	public:
		DURINED_API explicit DTransBuffer(const FObjectInitializer& ObjectInitializer);
		DURINED_API auto Begin(std::string_view Context, std::string_view Description) -> int32 override;
		DURINED_API auto End() -> int32 override;
		DURINED_API auto Cancel(int32 StartIndex = 0) -> void override;
		auto IsActive() const -> bool override { return ActiveCount != 0; }
		DURINED_API auto StoreUndo(DObject* Object, std::unique_ptr<Editor::FCommandChange> Change) -> Editor::FTransactorResult override;
		using DTransactor::StoreUndo;
		DURINED_API auto Undo(bool bCanRedo = true) -> bool override;
		DURINED_API auto UndoExpected(Editor::FTransactionId ExpectedId) -> Editor::FTransactorResult override;
		DURINED_API auto Redo() -> bool override;
		DURINED_API auto RedoExpected(Editor::FTransactionId ExpectedId) -> Editor::FTransactorResult override;
		DURINED_API auto Reset(std::string_view Reason = {}) -> Editor::FTransactorResult override;
		auto CanTransact() const -> bool override { return State == Editor::ETransactorState::Idle || State == Editor::ETransactorState::Recording; }
		DURINED_API auto SetPrimaryUndoObject(DObject* Object) -> void override;
		DURINED_API auto GetTransactionDetails(
			Editor::FTransactionId TransactionId,
			Editor::ETransactionOperation Operation) const -> std::string override;
		DURINED_API auto DiscardCustomChangesByModule(std::string_view ModuleName)
			-> Editor::FTransactorResult override;
		DURINED_API auto CanUndo(std::string* Reason = nullptr) const -> bool override;
		DURINED_API auto CanRedo(std::string* Reason = nullptr) const -> bool override;
		DURINED_API auto DispatchEvents() -> void override;
		auto GetQueueLength() const -> size_t override { return History.size(); }
		auto GetTransaction(size_t Index) const -> const Editor::FTransaction* override { return Index < History.size() ? &History[Index] : nullptr; }
		DURINED_API auto GetMountedContentMutationRevision() const -> uint64 override;
		DURINED_API auto NotifyMountedContentMutation() -> void override;
		DURINED_API auto NotifyMountedContentMutation(FContentChangeBatch Changes) -> void override;
		DURINED_API auto CaptureMountedContentChanges(uint64 FromRevision) const -> FContentChangeBatch override;
		DURINED_API auto EstablishSavedState(DPackage& Package) -> void override;
		DURINED_API auto MarkSaved(DPackage& Package) -> void override;
		DURINED_API auto InvalidateSavedState(DPackage& Package) -> void override;
		DURINED_API auto GetPackageRevisionState(const DPackage& Package) const
			-> std::optional<Editor::FPackageRevisionState> override;
		DURINED_API auto ForgetPackage(DPackage& Package) -> void override;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		DURINED_API auto BeginDestroy() -> void override;

		auto GetState() const -> Editor::ETransactorState { return State; }
		auto GetMemoryBudget() const -> size_t { return MemoryBudget; }
		// Matches UE: counts transactions already undone and available for Redo.
		auto GetUndoCount() const -> size_t override { return History.size() - Cursor; }
		auto GetOwnedBytes() const -> size_t { return OwnedBytes; }
		DURINED_API auto GetUndoId() const -> Editor::FTransactionId override;
		DURINED_API auto GetRedoId() const -> Editor::FTransactionId override;
		DURINED_API auto GetUndoContext(bool bCheckWhetherUndoPossible = true) const -> Editor::FTransactionContext override;
		DURINED_API auto GetRedoContext() const -> Editor::FTransactionContext override;

	private:
		friend class Editor::FTransaction;
		auto GetRecordingId() const -> Editor::FTransactionId override { return ActiveCount && Pending ? Pending->GetId() : 0; }
		DURINED_API auto CaptureObject(DObject* Object) -> bool;
		friend class FTransactorReloadParticipant;
		DURINED_API auto ReportChangeFailure(std::unique_ptr<Editor::FCommandChange> Change, Editor::FCommandChangeError Error) -> Editor::FTransactorResult override;

		DURINED_API auto Record(Editor::FTransactionObjectRecord Record) -> std::expected<uint64, Editor::FTransactorResult> override;
		DURINED_API auto UpdateRecord(uint64 RecordId, Editor::FTransactionObjectRecord Record) -> Editor::FTransactorResult override;
		auto ReleaseRecording() -> void;
		struct FTrackedPackageState
		{
			Editor::FPersistentObjectRef Package;
			Editor::FRevisionId CurrentRevision = 0;
			Editor::FRevisionId SavedRevision = 0;
			bool bCheckpointValid = false;
			uint64 ObservedEditRevision = 0;
		};

		auto RefreshOutsideEdits() -> void;
		auto HandleApplyFailure(const Editor::FTransaction& Transaction, Editor::FTransactionApplyError Error) -> Editor::FTransactorResult;
		auto CheckThread() const -> void;
		auto Reject(Editor::ETransactorRejectionReason Reason, uint64 RequestedId = 0,
			std::string_view ModuleName = {}) const -> Editor::FTransactorResult;
		auto FinalizePending() -> Editor::FTransactorResult;
		auto FindTransaction(Editor::FTransactionId TransactionId)
			-> Editor::FTransaction*;
		auto FindTransaction(Editor::FTransactionId TransactionId) const
			-> const Editor::FTransaction*;
		auto RecalculateOwnedBytes() -> bool;
		auto TrimHistoryToBudget() -> void;
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

		size_t MemoryBudget = 64u * 1024u * 1024u;
		Editor::ETransactorState State = Editor::ETransactorState::Idle;
		Editor::FTransactionId NextTransactionId = 1;
		int32 ActiveCount = 0;
		std::string ResetReason;
		std::optional<Editor::FTransaction> Pending;
		std::vector<Editor::FTransaction> History;
		size_t Cursor = 0;
		size_t OwnedBytes = 0;
		std::vector<Editor::FTransactionEvent> Events;
		Editor::FRevisionId NextRevision = 1;
		uint64 MountedContentMutationRevision = 1;
		FContentChangeJournal MountedContentChanges;
		std::unordered_map<DPackage*, FTrackedPackageState> PackageStates;

		friend struct FTransBufferTestAccess;
	};

	// Prepares whole-transaction retirement for every history entry that retains
	// a target graph. A busy buffer rejects through participant preparation.
	DURINED_API auto CreateTransactorReloadParticipant(
		DTransactor& Transactor, std::span<DPackage* const> Packages)
		-> std::shared_ptr<IPackageReloadParticipant>;
}

namespace Durin::Editor
{
	// Delineates one level of the active editor or explicitly supplied recording.
	class FScopedTransaction
	{
	public:
		DURINED_API explicit FScopedTransaction(std::string_view Description, bool bShouldActuallyTransact = true);
		DURINED_API FScopedTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject, bool bShouldActuallyTransact = true);
		DURINED_API FScopedTransaction(DTransactor* Transactor, FTransactionContext Context, bool bShouldActuallyTransact = true);
		DURINED_API ~FScopedTransaction();
		FScopedTransaction(const FScopedTransaction&) = delete;
		auto operator=(const FScopedTransaction&) -> FScopedTransaction& = delete;
		DURINED_API FScopedTransaction(FScopedTransaction&& Other) noexcept;
		DURINED_API auto operator=(FScopedTransaction&& Other) noexcept -> FScopedTransaction&;
		DURINED_API auto End() -> int32;
		DURINED_API auto Cancel() -> void;
		DURINED_API auto IsOutstanding() const -> bool;
	private:
		TStrongObjectPtr<DTransactor> Transactor;
		FTransactionId RecordingId = 0;
		int32 StartIndex = -1;
	};
}
