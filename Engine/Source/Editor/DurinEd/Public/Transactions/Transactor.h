#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "DObject/Object.h"
#include "Misc/ITransaction.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/TransactionObjectReference.h"
#include "DObject/WeakObjectPtr.h"
#include "Transactions/Transaction.h"
#include "DObject/TransactionDiff.h"
#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
#include "DObject/TransactionObjectEvent.h"
#include "Asset/PackageReload.h"

#include "Transactor.gen.h"

namespace Durin { class FArrayProperty; class DTransactor; class DTransBuffer; class FTransactorReloadParticipant; class DEditorEngine; }

namespace Durin
{
	// Selects a transaction owner for nested UI/asset callbacks without publishing GEditor.
	class FScopedEditorTransactionContext
	{
	public:
		DURINED_API explicit FScopedEditorTransactionContext(DEditorEngine* Editor);
		DURINED_API ~FScopedEditorTransactionContext();
		FScopedEditorTransactionContext(const FScopedEditorTransactionContext&) = delete;
		auto operator=(const FScopedEditorTransactionContext&) -> FScopedEditorTransactionContext& = delete;
	private:
		TWeakObjectPtr<DEditorEngine> Previous;
		bool bPreviousBound = false;
	};
	DURINED_API auto GetTransactionEditor() -> DEditorEngine*;


	// Identifies the current non-reentrant operation performed by a transactor.
	enum class ETransactorState : uint8
	{
		Idle,
		Recording,
		Undoing,
		Redoing,
		Destroying,
		RecoveryRequired,
		Resetting,
		Inspecting,
		Finalizing,
	};

	enum class ETransactionApplyFailure : uint8
	{
		ValidationFailed,
		Restored,
		RecoveryRequired
	};

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

	DURINED_API auto ResolveEditorTransactor(ETransactionMode Mode) -> std::expected<DTransactor*, std::string>;

	struct FTransactionPackageRevisionTransition
	{
		FPersistentObjectRef Package;
		FRevisionId BeforeRevision = 0;
		FRevisionId AfterRevision = 0;
		FRevisionId InitialSavedRevision = 0;
		auto operator==(const FTransactionPackageRevisionTransition&) const -> bool = default;
	};

	struct FTransactionObjectRecordError
	{
		FObjectKey Owner;
		std::string SerializationCause;
		bool bRecoveryRequired = false;
	};
	DURINED_API auto FormatTransactionObjectRecordError(const FTransactionObjectRecordError& Error) -> std::string;

	// Owns one immutable candidate or retained transaction and all of its record data.
	class FTransaction : public ITransaction
	{
		friend class ::Durin::DTransBuffer;
		friend class ::Durin::FTransactorReloadParticipant;
	protected:
		// One canonical whole-object serialized owner, captured before mutation.
		class FWholeObjectData
		{
		public:
			DURINED_API static auto CaptureWhole(DObject& Object)
				-> std::expected<FWholeObjectData, FTransactionObjectRecordError>;
			DURINED_API auto IsNoOp() const -> bool;
			DURINED_API auto HasExpired() const noexcept -> bool;
			DURINED_API auto Validate() const -> std::expected<void, FTransactionObjectRecordError>;
			DURINED_API auto CaptureAfter() -> std::expected<void, FTransactionObjectRecordError>;
			DURINED_API auto Apply(bool bBefore, EPropertyChangeOrigin Origin, bool bUseHistoricalState = true) -> std::expected<void, FTransactionObjectRecordError>;
			DURINED_API auto PrepareReplay() -> std::expected<void, FTransactionObjectRecordError>;
			DURINED_API auto CommitReplay(bool bBefore) -> void;
			// Build after native completion; replay's opposite side owns the live inverse.
			DURINED_API auto BuildObjectEvents(const FTransactionContext& Context,
				ETransactionObjectEventType Type, ETransactionOperation Operation) const
				-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
			DURINED_API auto Snapshot(const FTransactionContext& Context, std::span<const FProperty* const> Selected)
				-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
			auto ClearSnapshot() -> void { SnapshotState.reset(); std::vector<FName>().swap(SelectedSnapshotProperties); }
			auto ClearPreparedReplay(bool bClearAnnotation = true) -> void
			{
				ReplayState.reset();
				if (bClearAnnotation) CompletionAnnotation.reset();
			}
			auto GetReplayAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation> { return CompletionAnnotation; }
			DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
			auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
			DURINED_API auto CreateReplacement(const FObjectReplacementMap& Map) const
				-> std::expected<FWholeObjectData, std::string>;
			DURINED_API auto HasSameState(const FWholeObjectData& Other) const -> bool;
			DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;

		private:
			friend class FTransaction;
			auto ValidateSource(bool bUndo) const -> FTransactionCaptureResult;
			// UE's editor FSerializedObject is distinct from the runtime byte payload.
			struct FSerializedObject
			{
				FDiffableObject Observation;
				std::shared_ptr<ITransactionObjectAnnotation> Annotation;
				auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
				auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
				auto Validate(DObject& Object) const -> FTransactionCaptureResult;
				auto GetAnnotation() const -> const std::shared_ptr<ITransactionObjectAnnotation>& { return Annotation; }
				// A transient transfer view; identity and immutable allocations have one owner.
				auto GetSerializedCapture() const -> FSerializedObjectCapture
				{ return {Observation.SerializedObject, Observation.ObjectInfo, Observation.SerializationContext}; }
				auto HasSameSchema(const FSerializedObject& Other) const -> bool
				{ return Observation.ObjectInfo.Class == Other.Observation.ObjectInfo.Class
					&& Observation.SerializationContext && Other.Observation.SerializationContext
					&& Observation.SerializationContext->IsCompatibleWith(*Other.Observation.SerializationContext); }
			};
			static auto Capture(DObject& Object, const FSerializedObject* Policy = nullptr,
				const std::shared_ptr<ITransactionObjectAnnotation>* AnnotationPolicy = nullptr)
				-> std::expected<FSerializedObject, std::string>;
			auto Reject(std::string Message, bool bRecovery = false) const -> std::unexpected<FTransactionObjectRecordError>;
			FPersistentObjectRef Target;
			std::optional<FSerializedObject> BeforeState;
			std::optional<FSerializedObject> AfterState;
			std::optional<FSerializedObject> ReplayState;
			std::shared_ptr<ITransactionObjectAnnotation> CompletionAnnotation;
			std::optional<FSerializedObject> SnapshotState;
			std::vector<FName> SelectedSnapshotProperties;
		};

		// UE-style operation record. Resolve reflected storage anew after every vector
		// relocation; only the affected elements are owned by the history entry.
		class FArrayData
		{
		public:
			DURINED_API static auto Capture(const FTransactionArrayCapture& Request)
				-> std::expected<FArrayData, std::string>;
			DURINED_API auto HasExpired() const noexcept -> bool;
			DURINED_API auto Validate() const -> FTransactionCaptureResult;
			DURINED_API auto Apply(bool bCompensate, EPropertyChangeOrigin Origin, bool* OutRecoveryRequired) -> FTransactionCaptureResult;
			DURINED_API auto CommitReplay() -> void;
			auto ClearPreparedReplay() -> void { Inverse.reset(); CompletionAnnotation.reset(); InverseAnnotation.reset(); bHasCompletion = false; }
			auto IsNoOp() const -> bool { return Count == 0; }
			auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
			auto GetReplayAnnotation() const -> const std::shared_ptr<ITransactionObjectAnnotation>& { return CompletionAnnotation; }
			auto HasReplayCompletion() const -> bool { return bHasCompletion; }
			DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
			DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;
			DURINED_API auto CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FArrayData, std::string>;
			DURINED_API auto BuildObjectEvents(const FTransactionContext& Context,
				ETransactionObjectEventType Type, ETransactionOperation Operation) const
				-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
			auto HasSameState(const FArrayData& Other) const -> bool
			{
				return Target == Other.Target && Class == Other.Class && Member == Other.Member && MemberName == Other.MemberName
					&& MemberIndex == Other.MemberIndex && Index == Other.Index && Count == Other.Count && Oper == Other.Oper
					&& StructPath == Other.StructPath && Data == Other.Data && Inverse == Other.Inverse && Annotation == Other.Annotation
					&& InverseAnnotation == Other.InverseAnnotation && CompletionAnnotation == Other.CompletionAnnotation && bHasCompletion == Other.bHasCompletion;
			}

		private:
			struct FElement
			{
				FPropertyValueSnapshotPayload Payload;
				std::vector<FPersistentObjectRef> HardReferences;
				auto operator==(const FElement&) const -> bool = default;
			};
			using FRange = std::vector<FElement>;
			auto ResolveContainer() const -> void*;
			auto CaptureRange(size_t Num) const -> std::expected<FRange, std::string>;
			FPersistentObjectRef Target, Class;
			const FArrayProperty* Member = nullptr;
			FName MemberName;
			std::vector<const FProperty*> StructPath;
			uint32 MemberIndex = 0;
			size_t Index = 0, Count = 0;
			int8 Oper = 0;
			FRange Data;
			std::optional<FRange> Inverse;
			std::shared_ptr<ITransactionObjectAnnotation> Annotation, InverseAnnotation, CompletionAnnotation;
			bool bHasCompletion = false;
		};

		// One ordered entry for SaveObject, SaveArray or StoreUndo.
		class FObjectRecord
		{
		public:
			explicit FObjectRecord(FWholeObjectData Record)
				: Data(std::move(Record)) {}
			explicit FObjectRecord(FArrayData Record) : Data(std::move(Record)) {}
			DURINED_API explicit FObjectRecord(std::unique_ptr<FCommandChange> Change, DObject* Object = nullptr);

			DURINED_API auto HasExpired() const noexcept -> bool;
			DURINED_API auto Validate() const -> FTransactionCaptureResult;
			DURINED_API auto CaptureAfter() -> FTransactionCaptureResult;
			auto IsNoOp() const -> bool;
			DURINED_API auto Apply(
				bool bBefore,
				EPropertyChangeOrigin Origin, bool* OutRecoveryRequired = nullptr, bool bUseHistoricalState = true) -> FTransactionCaptureResult;
			DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			DURINED_API auto GetDetails(ETransactionOperation Operation) const -> std::string;
			DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
			DURINED_API auto IsOwnedByModule(std::string_view ModuleName) const -> bool;
			DURINED_API auto GetModifiedObjects() const -> std::vector<DObject*>;
			DURINED_API auto PrepareReplay() -> FTransactionCaptureResult;
			DURINED_API auto CommitReplay(bool bBefore) -> void;
			DURINED_API auto ClearPreparedReplay() -> void;
			DURINED_API auto CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FObjectRecord, std::string>;
			DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;

		private:
			friend class FTransaction;
			friend class DTransBuffer;
			friend class ::Durin::FTransactorReloadParticipant;
			auto GetReplayAnnotation() const -> std::pair<DObject*, std::shared_ptr<ITransactionObjectAnnotation>>;
			std::variant<FWholeObjectData, FArrayData,
				std::unique_ptr<FCommandChange>> Data;
			FPersistentObjectRef CustomObject;
			FTransactionObjectReferenceSet ModifiedObjects;
			FTransactionObjectReferenceSet DeclaredPackages;
			bool bReplayExpired = false;
		};
	public:
		DURINED_API FTransaction(FTransactionId Id, FTransactionContext Context, DTransBuffer* RecordingOwner = nullptr);
		FTransaction(const FTransaction&) = delete;
		auto operator=(const FTransaction&) -> FTransaction& = delete;
		FTransaction(FTransaction&&) noexcept = default;
		auto operator=(FTransaction&&) noexcept -> FTransaction& = default;

		auto GetId() const -> FTransactionId { return Id; }
		DURINED_API auto GetContext() const -> FTransactionContext override;
		DURINED_API auto SetPrimaryObject(DObject* Object) -> void override;
		DURINED_API auto BeginOperation() -> FTransactionCaptureResult override;
		DURINED_API auto EndOperation() -> FTransactionCaptureResult override;
		DURINED_API auto StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change) -> FTransactionCaptureResult override;
		DURINED_API auto SnapshotObject(DObject* Object, std::span<const FProperty* const> Properties = {}) -> FTransactionCaptureResult override;
		DURINED_API auto SaveArray(const FTransactionArrayCapture& Capture) -> FTransactionCaptureResult override;
		DURINED_API auto ContainsPieObjects() const -> std::expected<bool, std::string> override;
		auto GetRecordCount() const -> size_t { return Records.size(); }
		DURINED_API auto RemoveNoOpRecords() -> void;
		DURINED_API auto SaveObject(DObject* Object) -> void override;
		DURINED_API auto ContainsObject(const DObject* Object) const -> bool override;
		DURINED_API auto Finalize() -> FTransactionCaptureResult override;
		auto IsBusy() const -> bool { return bBusy; }
		DURINED_API auto HasExpired() const noexcept -> bool;
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
		DURINED_API auto IsOwnedByModule(std::string_view ModuleName) const -> bool;
		// Includes modified packages and collector-enumerated hard lifetime dependencies.
		DURINED_API auto ReferencesPackage(const DPackage& Package) const -> bool;

	private:
		friend class DTransBuffer;
		struct FReplayResult
		{
			std::vector<size_t> AppliedRecords;
			bool bDiscardHistory = false;
			std::unordered_map<DObject*, std::shared_ptr<ITransactionObjectAnnotation>> Annotations;
			// Only packages with skipped records need special checkpoint handling.
			std::unordered_map<DPackage*, bool> PackagesWithSkippedRecords;
			std::string Details;
		};
		auto Apply(bool bUndo, EPropertyChangeOrigin Origin, FReplayResult& Replay) -> FTransactionApplyResult;
		auto ValidateReplayRecords(std::span<const size_t> Indices) const -> FTransactionApplyResult;
		auto RetireExpiredRecords() -> void;
		auto DispatchObjectEvents(ETransactionObjectEventType Type, ETransactionOperation Operation,
			FWholeObjectData* SnapshotRecord = nullptr, std::span<const FProperty* const> Selected = {},
			const FReplayResult* Replay = nullptr) -> FTransactionCaptureResult;
		mutable bool bBusy = false;
		bool bFinalized = false;
		bool bObserverRecoveryRequired = false;
		struct FRecordingFailure
		{
			std::string Reason;
			std::vector<FObjectKey> Packages;
		};
		std::unique_ptr<FRecordingFailure> RecordingFailure;
		auto InvalidateRecording(std::string Reason, DPackage* Package) -> void;
		friend struct FTransBufferTestAccess;
		DURINED_API auto AddRecord(std::unique_ptr<FCommandChange> Change, DObject* Object = nullptr) -> uint64;
		struct FRecordedObject
		{
			size_t WholeRecord = 0; // One-based position in Records.
			bool bCustomOwner = false;
		};
		using FObjectIndex = std::unordered_map<FPersistentObjectRef, FRecordedObject>;
		static auto IndexRecord(FObjectIndex& Index, const FObjectRecord& Record, size_t Position) -> void;
		auto GetCaptureIndex() -> FObjectIndex&;
		auto AppendRecord(FObjectRecord&& Record) -> size_t;
		auto FindWholeRecord(DObject* Object) -> size_t;
		std::unique_ptr<FObjectIndex> CaptureIndex;
		DTransBuffer* RecordingOwner = nullptr;
		FTransactionId Id = 0;
		FTransactionContext Context;
		FPersistentObjectRef PrimaryObject;
		uint64 OperationId = 0;
		std::vector<FObjectRecord> Records;
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
		auto GetHistoryId() const -> uint64 { return HistoryId; }
		DURINED_API virtual auto Begin(std::string_view Context, std::string_view Description) -> int32 = 0;
		DURINED_API virtual auto End() -> int32 = 0;
		DURINED_API virtual auto Cancel(int32 StartIndex = 0) -> void = 0;
		DURINED_API virtual auto IsActive() const -> bool = 0;
		DURINED_API virtual auto StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change, std::string* OutError = nullptr) -> bool = 0;
		auto StoreUndo(std::unique_ptr<FCommandChange> Change, std::string* OutError = nullptr) -> bool
		{ return StoreUndo(nullptr, std::move(Change), OutError); }
		DURINED_API virtual auto Undo(bool bCanRedo = true) -> bool = 0;
		// Delayed history actions must supply the ID they were created for.
		DURINED_API virtual auto UndoExpected(FTransactionId ExpectedId, std::string* OutError = nullptr) -> bool = 0;
		DURINED_API virtual auto Redo() -> bool = 0;
		DURINED_API virtual auto RedoExpected(FTransactionId ExpectedId, std::string* OutError = nullptr) -> bool = 0;
		DURINED_API virtual auto Reset(std::string_view Reason = {}) -> void = 0;
		DURINED_API virtual auto CanTransact() const -> bool = 0;
		DURINED_API virtual auto SetPrimaryUndoObject(DObject* Object) -> void = 0;
		DURINED_API virtual auto DiscardCustomChangesByModule(std::string_view ModuleName) -> bool = 0;
		DURINED_API virtual auto CanUndo(std::string* Reason = nullptr) const -> bool = 0;
		DURINED_API virtual auto CanRedo(std::string* Reason = nullptr) const -> bool = 0;
		DURINED_API virtual auto SetUndoBarrier() -> void = 0;
		DURINED_API virtual auto RemoveUndoBarrier() -> void = 0;
		DURINED_API virtual auto ClearUndoBarriers() -> void = 0;
		virtual auto GetCurrentUndoBarrier() const -> int32 = 0;
		// Inspection failure conservatively reports possible PIE references.
		virtual auto ContainsPieObjects() const -> bool = 0;
		using FTransactionListener = std::function<void(const FTransactionEvent&)>;
		DURINED_API auto AddTransactionListener(FTransactionListener Listener) -> uint64;
		DURINED_API auto RemoveTransactionListener(uint64 Handle) -> void;
		DURINED_API virtual auto DispatchEvents() -> void = 0;
		DURINED_API virtual auto GetQueueLength() const -> size_t = 0;
		virtual auto GetUndoSize() const -> size_t = 0;
		DURINED_API virtual auto GetTransaction(size_t Index) const -> const FTransaction* = 0;
		DURINED_API virtual auto GetUndoContext(bool bCheckWhetherUndoPossible = true) const -> FTransactionContext = 0;
		DURINED_API virtual auto GetRedoContext() const -> FTransactionContext = 0;
		virtual auto GetUndoCount() const -> size_t = 0;
		DURINED_API virtual auto EstablishSavedState(DPackage& Package) -> void = 0;
		DURINED_API virtual auto MarkSaved(DPackage& Package) -> void = 0;
		DURINED_API virtual auto InvalidateSavedState(DPackage& Package) -> void = 0;
		DURINED_API virtual auto GetPackageRevisionState(const DPackage& Package) const
			-> std::optional<FPackageRevisionState> = 0;
		DURINED_API virtual auto ForgetPackage(DPackage& Package) -> void = 0;

	protected:
		DURINED_API explicit DTransactor(const FObjectInitializer& ObjectInitializer);
		std::map<uint64, FTransactionListener> Listeners;
		uint64 NextListener = 1;
	private:
		const uint64 HistoryId = AllocateTransactionOperationId();
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
		DURINED_API auto StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change, std::string* OutError = nullptr) -> bool override;
		using DTransactor::StoreUndo;
		DURINED_API auto Undo(bool bCanRedo = true) -> bool override;
		DURINED_API auto UndoExpected(FTransactionId ExpectedId, std::string* OutError = nullptr) -> bool override;
		DURINED_API auto Redo() -> bool override;
		DURINED_API auto RedoExpected(FTransactionId ExpectedId, std::string* OutError = nullptr) -> bool override;
		DURINED_API auto Reset(std::string_view Reason = {}) -> void override;
		auto CanTransact() const -> bool override { return (!GUndo || (Pending && GUndo == &*Pending)) && (!Pending || !Pending->IsBusy()) && (State == ETransactorState::Idle || State == ETransactorState::Recording); }
		DURINED_API auto SetPrimaryUndoObject(DObject* Object) -> void override;
		DURINED_API auto DiscardCustomChangesByModule(std::string_view ModuleName) -> bool override;
		DURINED_API auto CanUndo(std::string* Reason = nullptr) const -> bool override;
		DURINED_API auto CanRedo(std::string* Reason = nullptr) const -> bool override;
		DURINED_API auto SetUndoBarrier() -> void override;
		DURINED_API auto RemoveUndoBarrier() -> void override;
		DURINED_API auto ClearUndoBarriers() -> void override;
		auto GetCurrentUndoBarrier() const -> int32 override
		{ return UndoBarriers.empty() ? -1 : static_cast<int32>(UndoBarriers.back()); }
		DURINED_API auto ContainsPieObjects() const -> bool override;
		// Applies to the next successful commit; never evicts pending or redo data.
		DURINED_API auto SetMemoryBudget(size_t Bytes) -> bool;
		DURINED_API auto DispatchEvents() -> void override;
		auto GetQueueLength() const -> size_t override { return History.size(); }
		auto GetTransaction(size_t Index) const -> const FTransaction* override { return State != ETransactorState::Finalizing && Index < History.size() ? &History[Index] : nullptr; }
		DURINED_API auto EstablishSavedState(DPackage& Package) -> void override;
		DURINED_API auto MarkSaved(DPackage& Package) -> void override;
		DURINED_API auto InvalidateSavedState(DPackage& Package) -> void override;
		DURINED_API auto GetPackageRevisionState(const DPackage& Package) const
			-> std::optional<FPackageRevisionState> override;
		DURINED_API auto ForgetPackage(DPackage& Package) -> void override;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		DURINED_API auto BeginDestroy() -> void override;
		// Native reinstancing hook; graph publication and external owners remain
		// with the caller. Uses the same checked history participant as package reload.
		DURINED_API auto OnObjectsReinstanced(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;

		auto GetMemoryBudget() const -> size_t { return MemoryBudget; }
		// Matches UE: counts transactions already undone and available for Redo.
		auto GetUndoCount() const -> size_t override { return History.size() - Cursor; }
		auto GetUndoSize() const -> size_t override { return OwnedBytes; }
		DURINED_API auto GetUndoContext(bool bCheckWhetherUndoPossible = true) const -> FTransactionContext override;
		DURINED_API auto GetRedoContext() const -> FTransactionContext override;

	private:
		friend class FTransaction;
		friend class FTransactorReloadParticipant;

		auto AppendCommandRecord(FTransaction::FObjectRecord&& Record) -> void;
		auto ReleaseRecording() -> void;
		struct FTrackedPackageState
		{
			FPersistentObjectRef Package;
			FRevisionId CurrentRevision = 0;
			FRevisionId SavedRevision = 0;
			uint64 ObservedEditRevision = 0;
			auto operator==(const FTrackedPackageState&) const -> bool = default;
		};

		auto RefreshOutsideEdits() -> void;
		auto ClearReplayHistory(const FTransaction& Transaction, ETransactionEventType Type,
			ETransactionOperation Operation, std::string Reason, bool bApplied) -> void;
		auto HandleApplyFailure(const FTransaction& Transaction, FTransactionApplyError Error,
			std::string* OutError = nullptr, bool bMeasureHistory = true) -> bool;
		auto CheckThread() const -> void;
		auto FinalizePending(bool& bPublicationStarted) -> void;
		auto DiscardPending(FTransactionId Id, ETransactionEventType EventType, std::string Message, bool bUnknownEffects = false) -> void;
		auto DiscardIncompleteRecording() -> void;
		auto ClearHistory(std::string Reason) -> void;
		auto RecalculateOwnedBytes() -> bool;
		auto MeasureOwnedBytes(size_t Count) const -> std::optional<size_t>;
		auto RemapUndoBarriers(size_t First, size_t Count) -> void;
		auto QueueEvent(ETransactionEventType Type,
			const FTransaction& Transaction,
			std::optional<std::string> Details = std::nullopt) -> void;
		auto AllocateRevision() -> FRevisionId;
		auto PreparePackageTransitions(FTransaction& Transaction) -> void;
		auto FindReplayIndex(bool bUndo, bool bRespectUndoBarrier = true) const -> std::optional<size_t>;
		auto CanReplayInHost(std::string* Reason) const -> bool;
		auto Replay(bool bUndo, std::optional<FTransactionId> ExpectedId = std::nullopt,
			bool bCanRedo = true, std::string* OutError = nullptr) -> bool;
		auto ApplyPackageTransitions(const FTransaction& Transaction,
			bool bForward, const FTransaction::FReplayResult* Replay = nullptr) -> void;
		auto FindPackageState(const DPackage& Package) -> FTrackedPackageState*;
		auto FindPackageState(const DPackage& Package) const -> const FTrackedPackageState*;
		auto EnsurePackageState(DPackage& Package) -> FTrackedPackageState&;
		auto SynchronizeDirtyState(FTrackedPackageState& State) -> void;

		size_t MemoryBudget = 64u * 1024u * 1024u;
		mutable ETransactorState State = ETransactorState::Idle;
		FTransactionId NextTransactionId = 1;
		int32 ActiveCount = 0;
		std::string ResetReason;
		std::optional<FTransaction> Pending;
		std::vector<FTransaction> History;
		size_t Cursor = 0;
		std::vector<size_t> UndoBarriers;
		size_t OwnedBytes = 0;
		std::vector<FTransactionEvent> Events;
		FRevisionId NextRevision = 1;
		uint64 HistoryRevision = 0;
		std::unordered_map<DPackage*, FTrackedPackageState> PackageStates;

		friend struct FTransBufferTestAccess;
	};

	// Prepares retained history against the same checked graph map. Unsupported
	// state retires its whole transaction. Busy or changed buffers reject publication.
	DURINED_API auto CreateTransactorReloadParticipant(
		DTransactor& Transactor, std::span<DPackage* const> Packages)
		-> std::shared_ptr<IPackageReloadParticipant>;
}

namespace Durin
{
	// Unwind before the active editor is replaced.
	class FScopedTransaction
	{
	public:
		DURINED_API explicit FScopedTransaction(std::string_view Description, bool bShouldActuallyTransact = true);
		DURINED_API FScopedTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject, bool bShouldActuallyTransact = true);
		DURINED_API ~FScopedTransaction();
		FScopedTransaction(const FScopedTransaction&) = delete;
		auto operator=(const FScopedTransaction&) -> FScopedTransaction& = delete;
		DURINED_API auto Cancel() -> void;
		// Unwind other scopes from a canceled recording before starting a new one on this host.
		auto IsOutstanding() const -> bool { return StartIndex >= 0; }
	private:
		int32 StartIndex = -1;
	};
}
