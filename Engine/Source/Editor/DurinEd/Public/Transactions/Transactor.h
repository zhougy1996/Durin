#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Delegates/Delegate.h"
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



	DURINED_API auto ResolveEditorTransactor(bool bShouldTransact) -> std::expected<DTransactor*, std::string>;

	struct FTransactionPackageRecord
	{
		FPersistentObjectRef Package;
		bool bWasDirty = false;
		uint64 SaveCount = 0;
		auto operator==(const FTransactionPackageRecord&) const -> bool = default;
	};

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
				-> std::expected<FWholeObjectData, std::string>;
			DURINED_API auto IsNoOp() const -> bool;
			DURINED_API auto HasExpired() const noexcept -> bool;
			DURINED_API auto Validate() const -> FTransactionCaptureResult;
			DURINED_API auto CaptureAfter() -> void;
			DURINED_API auto Apply(EPropertyChangeOrigin Origin) -> void;
			DURINED_API auto CaptureInverse() -> void;
			// Build after native completion; replay's opposite side owns the live inverse.
			DURINED_API auto BuildObjectEvents(const FTransactionContext& Context,
				ETransactionObjectEventType Type, const std::shared_ptr<ITransactionObjectAnnotation>& Annotation) const
				-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
			DURINED_API auto Snapshot(const FTransactionContext& Context, std::span<const FProperty* const> Selected)
				-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
			auto ClearSnapshot() -> void { SnapshotState.reset(); std::vector<FName>().swap(SelectedSnapshotProperties); }
			auto GetReplayAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation> { return SerializedObjectFlip->Annotation; }
			DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
			auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
			DURINED_API auto Reinstance(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
			DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;

		private:
			friend class FTransaction;
			auto ValidateSource() const -> FTransactionCaptureResult;
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
			FPersistentObjectRef Target;
			std::optional<FSerializedObject> SerializedObject;
			std::optional<FSerializedObject> SerializedObjectFlip;
			std::optional<FSerializedObject> SnapshotState;
			bool bFinalized = false;
			std::optional<FTransactionObjectChange> FinalizedChange;
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
			auto ValidateSourceRange(bool bFirstLoad) const -> FTransactionCaptureResult;
			DURINED_API auto CaptureInverse() -> void;
			DURINED_API auto Apply(EPropertyChangeOrigin Origin) -> void;
			auto ClearPreparedReplay() -> void { Inverse.reset(); CompletionAnnotation.reset(); InverseAnnotation.reset(); ReplayChange.reset(); }
			auto IsNoOp() const -> bool { return Count == 0; }
			auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
			auto GetReplayAnnotation() const -> const std::shared_ptr<ITransactionObjectAnnotation>& { return CompletionAnnotation; }
			DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
			DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;
			DURINED_API auto Reinstance(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
			DURINED_API auto BuildObjectEvents(const FTransactionContext& Context,
				ETransactionObjectEventType Type) const
				-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;

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
			std::optional<FTransactionObjectChange> ReplayChange;
			std::shared_ptr<ITransactionObjectAnnotation> Annotation, InverseAnnotation, CompletionAnnotation;
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
			auto IsNoOp() const -> bool;
			DURINED_API auto Apply(
				bool bBefore,
				EPropertyChangeOrigin Origin) -> void;
			DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			DURINED_API auto GetDetails(bool bReplay) const -> std::string;
			DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
			DURINED_API auto IsOwnedByModule(std::string_view ModuleName) const -> bool;
			DURINED_API auto GetModifiedObjects() const -> std::vector<DObject*>;
			DURINED_API auto CaptureInverse() -> void;
			DURINED_API auto ClearPreparedReplay() -> void;
			DURINED_API auto Reinstance(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
			DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;

		private:
			friend class FTransaction;
			friend class DTransBuffer;
			friend class ::Durin::FTransactorReloadParticipant;
			auto GetReplayAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation>;
			std::variant<FWholeObjectData, FArrayData,
				std::unique_ptr<FCommandChange>> Data;
			FPersistentObjectRef CustomObject;
			FTransactionObjectReferenceSet ModifiedObjects;
			FTransactionObjectReferenceSet DeclaredPackages;
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
		DURINED_API auto Finalize() -> void override;
		auto IsBusy() const -> bool { return bBusy; }
		DURINED_API auto HasExpired() const noexcept -> bool;
		DURINED_API auto Validate() const -> std::expected<void, std::string>;
		DURINED_API auto Apply() -> void override;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetOwnedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto GetDetails() const -> std::string;
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
			std::vector<size_t> SkippedRecords;
			std::vector<std::pair<FObjectKey, bool>> LifecycleSources;
			// Only packages with skipped records need special dirty-state handling.
			std::unordered_map<DPackage*, bool> PackagesWithSkippedRecords;
			std::string Details;
		};
		auto PrepareReplay(bool bUndo, FReplayResult& Replay) const -> FTransactionCaptureResult;
		auto ApplyPrepared(bool bUndo, FReplayResult& Replay) -> void;
		bool bUndoNext = true;
		struct FChangedObject
		{
			FPersistentObjectRef Target;
			const FObjectRecord* Record;
			std::shared_ptr<ITransactionObjectAnnotation> Annotation;
		};
		using FObjectEvents = std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>;
		auto NotifyRecords(std::vector<FChangedObject>& ChangedObjects, ETransactionObjectEventType Type) -> void;
		auto DispatchObjectEvents(const FObjectEvents& Events) -> FTransactionCaptureResult;
		mutable bool bBusy = false;
		bool bFinalized = false;
		bool bNotifyingPreFinalize = false;
		bool bObjectEventFailed = false;
		std::optional<std::string> RecordingFailure;
		friend struct FTransBufferTestAccess;
		DURINED_API auto AddRecord(std::unique_ptr<FCommandChange> Change, DObject* Object = nullptr) -> uint64;
		struct FRecordedObject
		{
			size_t WholeRecord = 0; // One-based position in Records.
			bool bCustomOwner = false;
		};
		using FObjectIndex = std::unordered_map<FPersistentObjectRef, FRecordedObject>;
		static auto IndexRecord(FObjectIndex& Index, const FObjectRecord& Record, size_t Position) -> void;
		auto SavePackage(DPackage* Package) -> void;
		auto PreparePackageRecords() -> void;
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
		std::vector<FTransactionPackageRecord> PackageRecords;
	};


}

namespace Durin
{
	struct FTransBufferTestAccess;
	class FTransactorReloadParticipant;

	// Synchronous notifications for the lifetime of an outer recording.
	enum class ETransactionStateEventType : uint8
	{
		TransactionStarted, PreTransactionFinalized, TransactionFinalized, PostTransactionFinalized, TransactionCanceled,
		UndoRedoStarted, UndoRedoFinalized
	};

	// Requires a complete editor recording, history and package dirty-state implementation.
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
		// Synchronous, while the outer recording still accepts nested transactions.
		auto OnPreTransactionFinalized() -> TMulticastDelegate<void(const FTransactionContext&)>& { return PreTransactionFinalized; }
		auto OnTransactionStateChanged() -> TMulticastDelegate<void(const FTransactionContext&, ETransactionStateEventType)>& { return TransactionStateChanged; }
		auto OnUndoBufferChanged() -> TMulticastDelegate<void()>& { return UndoBufferChanged; }
		using FReplayDelegate = TMulticastDelegate<void(const FTransactionContext&, bool)>;
		auto OnUndo() -> FReplayDelegate& { return UndoDelegate; }
		auto OnRedo() -> FReplayDelegate& { return RedoDelegate; }
		DURINED_API virtual auto GetQueueLength() const -> size_t = 0;
		virtual auto GetUndoSize() const -> size_t = 0;
		DURINED_API virtual auto GetTransaction(size_t Index) const -> const FTransaction* = 0;
		DURINED_API virtual auto GetUndoContext(bool bCheckWhetherUndoPossible = true) const -> FTransactionContext = 0;
		DURINED_API virtual auto GetRedoContext() const -> FTransactionContext = 0;
		virtual auto GetUndoCount() const -> size_t = 0;
		DURINED_API virtual auto ForgetPackage(DPackage& Package) -> void = 0;

	protected:
		DURINED_API explicit DTransactor(const FObjectInitializer& ObjectInitializer);
		FReplayDelegate UndoDelegate;
		FReplayDelegate RedoDelegate;
	private:
		TMulticastDelegate<void(const FTransactionContext&)> PreTransactionFinalized;
		TMulticastDelegate<void(const FTransactionContext&, ETransactionStateEventType)> TransactionStateChanged;
		TMulticastDelegate<void()> UndoBufferChanged;
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
		auto CanTransact() const -> bool override { return !GIsTransacting && Guard == EHistoryGuard::None && (!GUndo || OwnsRecording()) && (!Pending || !Pending->IsBusy()); }
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
		// The next outer Begin can temporarily remove old entries to meet this budget.
		DURINED_API auto SetMemoryBudget(size_t Bytes) -> bool;
		auto GetQueueLength() const -> size_t override { return History.size(); }
		auto GetTransaction(size_t Index) const -> const FTransaction* override { return Guard != EHistoryGuard::Finalizing && Index < History.size() ? History[Index].get() : nullptr; }
		DURINED_API auto ForgetPackage(DPackage& Package) -> void override;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		DURINED_API auto BeginDestroy() -> void override;
		// Native reinstancing hook; graph publication and external owners remain
		// with the caller. Uses the same checked history participant as package reload.
		DURINED_API auto OnObjectsReinstanced(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;

		auto GetMemoryBudget() const -> size_t { return MemoryBudget; }
		// Matches UE: counts transactions already undone and available for Redo.
		auto GetUndoCount() const -> size_t override { return History.size() - Cursor; }
		DURINED_API auto GetUndoSize() const -> size_t override;
		DURINED_API auto GetUndoContext(bool bCheckWhetherUndoPossible = true) const -> FTransactionContext override;
		DURINED_API auto GetRedoContext() const -> FTransactionContext override;

	private:
		friend class FTransaction;
		friend class FTransactorReloadParticipant;

		// Mutating blocks reentry. Inspection also blocks recursive head queries,
		// and finalization hides entries while the buffer publishes or retires them.
		enum class EHistoryGuard : uint8 { None, Mutating, Notifying, Inspecting, Finalizing };
		struct FHistoryGuardScope;
		auto OwnsRecording() const -> bool { return ActiveCount > 0 && Pending && GUndo == Pending; }
		auto IsHistoryIdle() const -> bool { return Guard == EHistoryGuard::None && !Pending && !IsActive(); }
		auto CanAccessReplayHistory() const -> bool { return IsHistoryIdle() && !GUndo && !GIsTransacting; }
		auto CanInspectReplayHistory() const -> bool
		{ return CanAccessReplayHistory() || (Guard == EHistoryGuard::Notifying && ActiveCount == 0); }
		auto AppendCommandRecord(FTransaction::FObjectRecord&& Record) -> void;
		auto RestoreRecordingHistory() -> void;
		auto ReleaseRecording() -> void;
		auto CheckThread() const -> void;
		auto FinalizePending(FTransactionContext& PostContext) -> void;
		auto ClearHistory(std::string Reason) -> void;
		auto RemapUndoBarriers(size_t First, size_t Count) -> void;
		auto CanReplayInHost(std::string* Reason) const -> bool;
		auto Replay(bool bUndo, std::optional<FTransactionId> ExpectedId = std::nullopt,
			bool bCanRedo = true, std::string* OutError = nullptr) -> bool;

		size_t MemoryBudget = 64u * 1024u * 1024u;
		mutable EHistoryGuard Guard = EHistoryGuard::None;
		FTransactionId NextTransactionId = 1;
		int32 ActiveCount = 0;
		std::string ResetReason;
		FTransaction* Pending = nullptr;
		std::vector<std::unique_ptr<FTransaction>> RemovedTransactions;
		size_t PreviousUndoCount = 0;
		std::vector<std::unique_ptr<FTransaction>> History;
		size_t Cursor = 0;
		std::vector<size_t> UndoBarriers;

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
		DURINED_API auto IsRecordingCurrent() const -> bool;
		auto IsOutstanding() const -> bool { return StartIndex >= 0; }
	private:
		TWeakObjectPtr<DTransactor> RecordingOwner;
		FTransactionId RecordingId = 0;
		int32 StartIndex = -1;
	};
}
