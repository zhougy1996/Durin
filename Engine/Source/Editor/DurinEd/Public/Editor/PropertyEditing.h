#pragma once

#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
#include "DObject/ObjectValidation.h"
#include "DObject/ContainerOps.h"
#include "DObject/StrongObjectPtr.h"
#include "DurinEdAPI.h"
#include "Editor/Transaction.h"
#include "Editor/TransactionObjectRecord.h"
#include "Editor/Transactor.h"

namespace Durin
{
	class DObject;
	class FProperty;
}

namespace Durin::Editor
{
	struct FPropertyEditExtension
	{
		std::function<FObjectValidationResult(DObject&, FPropertyEditProposal&)> PreEdit;
		std::function<void(DObject&, const FPropertyChangedEvent&)> PostEdit;
	};

	using FPropertyEditExtensionHandle = uint64;

	DURINED_API auto RegisterPropertyEditExtension(FPropertyEditExtension Extension)
		-> FPropertyEditExtensionHandle;
	DURINED_API auto UnregisterPropertyEditExtension(FPropertyEditExtensionHandle Handle) -> void;

	// Identifies one stable traversal step from a reflected member to a nested value.
	struct FPropertyEditPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		FByteBuffer MapKeyData;
		FPropertyValueSnapshotPayload MapKey;
	};

	enum class EPropertyEditPathError : uint8
	{
		None, MissingOwner, IncompleteTarget, SnapshotIndex, Endpoints, EmptySegment,
		UnexpectedKeyData, SnapshotRoot, ArrayProperty, ArrayCount, ArrayIndex,
		ArrayAccess, MapSnapshot, MapTraversal, MapCapture, MapMissing, MapSelection,
		Selector, Unresolved, Empty
	};
	struct FPropertyEditPathContext
	{
		std::string Property;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		FByteBuffer KeyData;
		FPropertyValueSnapshotPayload MapKey;
	};
	struct FPropertyEditPathError
	{
		EPropertyEditPathError Code = EPropertyEditPathError::None;
		FObjectKey Owner;
		std::string Member;
		std::string Leaf;
		std::string Snapshot;
		bool HasSnapshotContainer = false;
		uint64 SnapshotArrayIndex = 0;
		std::vector<FPropertyEditPathContext> Path;
		uint64 PathIndex = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<EContainerOpResult> ContainerCause;
		std::optional<FPropertySnapshotError> SnapshotCause;
	};
	struct FPropertyEditPathResult
	{
		FPropertyEditPathError Error;
		explicit operator bool() const { return Error.Code == EPropertyEditPathError::None; }
	};
	DURINED_API auto FormatPropertyEditPathError(const FPropertyEditPathError& Error) -> std::string;

	enum class EPropertyValueDraftError : uint8
	{
		None, MissingRoot, Accessors, Lifecycle, Capture, Storage, Restore, RootMismatch, Path
	};

	struct FPropertyValueDraftError
	{
		EPropertyValueDraftError Code = EPropertyValueDraftError::None;
		std::string Root;
		uint32 ArrayIndex = 0;
		bool HasContainer = false;
		bool HasLifecycle = false;
		uint64 ValueSize = 0;
		uint64 ValueAlignment = 0;
		std::string RequestedRoot;
		uint32 RequestedArrayIndex = 0;
		std::optional<FPropertySnapshotError> SnapshotCause;
		std::optional<FPropertyValueError> ValueCause;
		std::optional<FPropertyEditPathError> PathCause;
	};

	struct FPropertyValueDraftResult
	{
		FPropertyValueDraftError Error;
		explicit operator bool() const { return Error.Code == EPropertyValueDraftError::None; }
	};

	DURINED_API auto FormatPropertyValueDraftError(const FPropertyValueDraftError& Error) -> std::string;

	enum class EPropertyMutationError : uint8
	{
		None, RecursiveEdit, MissingValue, CaptureBefore, Draft, ExtensionValidation,
		ObjectValidation, DeferredUnavailable, Publication, CaptureAfter
	};
	struct FPropertyMutationError
	{
		EPropertyMutationError Code = EPropertyMutationError::None;
		FObjectKey Owner;
		std::string Member;
		EPropertyChangePhase Phase = EPropertyChangePhase::Interactive;
		EPropertyChangeOrigin Origin = EPropertyChangeOrigin::Edit;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;
		std::optional<FPropertyValueDraftError> DraftCause;
		std::optional<FPropertySnapshotError> SnapshotCause;
		std::optional<FObjectValidationError> ValidationCause;
		std::optional<FPropertySnapshotError> RollbackCause;
		std::optional<FPropertySnapshotError> RecoveryCaptureCause;
	};
	struct FPropertyMutationResult
	{
		FPropertyMutationError Error;
		explicit operator bool() const { return Error.Code == EPropertyMutationError::None; }
	};
	DURINED_API auto FormatPropertyMutationError(const FPropertyMutationError& Error) -> std::string;

	// Describes a reflected edit using a stable snapshot root and logical path.
	struct FPropertyEditTarget
	{
		DObject* Object = nullptr;
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		// Nested container elements can move after an array resize or map rehash.
		// Transactions therefore snapshot a stable ancestor, normally the object-owned member.
		const FProperty* SnapshotProperty = nullptr;
		void* SnapshotContainer = nullptr;
		uint32 SnapshotArrayIndex = 0;
		std::vector<FPropertyEditPathSegment> Path;
		// Logical identity distinguishes independently edited values that intentionally
		// share one stable snapshot root, such as GUID-addressed array entries.
		FByteBuffer LogicalIdentity;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;

		DURINED_API auto Validate() const -> FPropertyEditPathResult;
		DURINED_API static auto ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex = 0) -> FPropertyEditTarget;
		DURINED_API auto ForStructMember(const FProperty* Property, uint32 ArrayIndex = 0) const -> FPropertyEditTarget;
		DURINED_API auto ForArrayElement(const FProperty* ElementProperty, uint64 ElementIndex) const -> FPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty, FByteBuffer SerializedKey) const -> FPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty,
			FPropertyValueSnapshotPayload KeySnapshot,
			FByteBuffer SerializedKey) const -> FPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty,
			const FPropertyValueSnapshot& KeySnapshot,
			FByteBuffer SerializedKey) const -> FPropertyEditTarget;

		// Includes storage identity and key values for same-target mutation recursion protection.
		DURINED_API auto IsSameMutationTarget(const FPropertyEditTarget& Other) const -> bool;
		// Ignores transient storage addresses when matching a retained UI draft.
		DURINED_API auto IsSameStableTarget(const FPropertyEditTarget& Other) const -> bool;
		// Treats the changing key value of one continuous map-key rename as the same edit.
		DURINED_API auto MatchesContinuousEdit(const FPropertyEditTarget& Other) const -> bool;
	};

	// Reports whether a reflected edit failed, changed nothing, or changed value.
	enum class EPropertyEditResult : uint8
	{
		Failed,
		NoChange,
		Changed,
		Pending,
	};

	enum class EPropertyEditSessionError : uint8
	{
		None, AlreadyActive, Inactive, Target, UnavailableOwner, Capture,
		Scope, Record, RecordAdmission, RecordUpdate, Mutation, Commit, Cancel
	};
	struct FPropertyEditSessionError
	{
		EPropertyEditSessionError Code = EPropertyEditSessionError::None;
		FObjectKey Owner;
		std::string Member;
		std::string Description;
		uint64 RecordId = 0;
		std::optional<FPropertyEditPathError> PathCause;
		std::optional<FPropertySnapshotError> SnapshotCause;
		std::optional<FTransactionObjectRecordError> RecordCause;
		std::optional<FPropertyMutationError> MutationCause;
		std::optional<FPropertyMutationError> RollbackCause;
		bool RollbackDeferred = false;
		std::optional<FTransactorResult> TransactorCause;
		std::optional<FTransactorResult> CleanupCause;
	};
	struct FPropertyEditOperationResult
	{
		EPropertyEditResult Disposition = EPropertyEditResult::NoChange;
		FPropertyEditSessionError Error;
		explicit operator bool() const { return Error.Code == EPropertyEditSessionError::None; }
		auto GetStatus() const -> EPropertyEditResult
		{ return Error.Code == EPropertyEditSessionError::None ? Disposition : EPropertyEditResult::Failed; }
	};
	DURINED_API auto FormatPropertyEditSessionError(const FPropertyEditSessionError& Error) -> std::string;

	// Coalesces continuous widget changes into one reflected-property transaction.
	class FPropertyEditSession
	{
	public:
		DURINED_API ~FPropertyEditSession();
		FPropertyEditSession() = default;
		FPropertyEditSession(const FPropertyEditSession&) = delete;
		auto operator=(const FPropertyEditSession&) -> FPropertyEditSession& = delete;

		DURINED_API auto Begin(
			const FPropertyEditTarget& InTarget,
			// An empty description uses "Edit <MemberProperty>" after target validation.
			std::string_view InDescription,
			DTransactor* InTransactor = nullptr
		) -> FPropertyEditOperationResult;
		DURINED_API auto Apply(const FPropertyValueSnapshot& ProposedValue) -> FPropertyEditOperationResult;
		DURINED_API auto Apply(const FPropertyValueSnapshotPayload& ProposedValue) -> FPropertyEditOperationResult;
		DURINED_API auto Commit() -> FPropertyEditOperationResult;
		DURINED_API auto Cancel() -> FPropertyEditOperationResult;

		auto IsActive() const -> bool { return bActive; }
		DURINED_API auto MatchesTarget(const FPropertyEditTarget& Other) const -> bool;
		auto HasChanges() const -> bool { return bActive && !(OriginalValue == CurrentValue); }
		auto HasPendingDeferredEdit() const -> bool { return bDeferredPending; }
		auto GetDescription() const -> std::string_view { return Description; }
		auto GetOriginalValue() const -> const FPropertyValueSnapshotPayload& { return OriginalValue; }
		auto GetCurrentValue() const -> const FPropertyValueSnapshotPayload& { return CurrentValue; }

	private:
		struct FDeferredOwnerState;
		auto CompleteDeferredEdit(
			FObjectValidationResult Validation,
			FPropertyValueSnapshotPayload ProposedValue) -> void;
		auto UpdateTransactorRecord() -> FPropertyEditOperationResult;
		auto Reject(EPropertyEditSessionError Code) const -> FPropertyEditOperationResult;
		auto Reset() -> void;

		FPropertyEditTarget Target;
		TStrongObjectPtr<DObject> TargetObject;
		FPropertyValueSnapshotPayload OriginalValue;
		FPropertyValueSnapshotPayload CurrentValue;
		std::string Description;
		DTransactor* Transactor = nullptr;
		std::optional<FScopedTransaction> TransactionScope;
		uint64 TransactionRecordId = 0;
		bool bActive = false;
		bool bDeferredPending = false;
		std::shared_ptr<FDeferredOwnerState> DeferredOwnerState;
		FPropertyEditDeferredCancel CancelDeferredEdit;
	};
}
