#pragma once

#include "CoreMinimal.h"

#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
#include "DObject/ObjectValidation.h"
#include "DObject/ContainerOps.h"
#include "DObject/StrongObjectPtr.h"
#include "DurinEdAPI.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	namespace PropertyEditor { class FPropertyEdit; }
	class FPropertyView;
	class DObject;
	class FProperty;
}

namespace Durin
{
	namespace PropertyEditor { class FPropertyEdit; }
	class FPropertyView;
	struct FPropertyEditExtension
	{
		std::function<std::expected<void, FObjectValidationError>(DObject&, FPropertyEditProposal&)> PreEdit;
		std::function<void(DObject&, const FPropertyChangedEvent&, bool bValueChanged)> PostEdit;
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


	DURINED_API auto FormatPropertyValueDraftError(const FPropertyValueDraftError& Error) -> std::string;

	enum class EPropertyMutationError : uint8
	{
		None, RecursiveEdit, MissingValue, CaptureBefore, Draft, ExtensionValidation,
		ObjectValidation, Publication, CaptureAfter
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

		DURINED_API auto Validate() const -> std::expected<void, FPropertyEditPathError>;
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

	// Coalesces continuous widget changes into one reflected-property transaction.
	class FPropertyEditSession
	{
	public:
		DURINED_API ~FPropertyEditSession();

		// Operations return success. Optional diagnostics are written on failure.
		FPropertyEditSession() = default;
		FPropertyEditSession(const FPropertyEditSession&) = delete;
		auto operator=(const FPropertyEditSession&) -> FPropertyEditSession& = delete;

		DURINED_API auto Begin(
			const FPropertyEditTarget& InTarget,
			// An empty description uses "Edit <MemberProperty>" after target validation.
			std::string_view InDescription,
			ETransactionMode Mode = ETransactionMode::Record,
			std::string* OutError = nullptr
		) -> bool;
		DURINED_API auto Begin(const FPropertyEditTarget& Target, std::string_view Description,
			DTransactor& Transactor, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshotPayload& ProposedValue, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> bool;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> bool;
		// Close a rejected/no-change edit without discarding an enclosing recording.
		// Applied previews must use Cancel instead.
		DURINED_API auto CancelUnchanged(std::string* OutError = nullptr) -> bool;

		auto IsActive() const -> bool { return bActive; }
		DURINED_API auto MatchesTarget(const FPropertyEditTarget& Other) const -> bool;
		auto HasChanges() const -> bool { return bActive && !(OriginalValue == CurrentValue); }
		auto GetDescription() const -> std::string_view { return Description; }
		auto GetOriginalValue() const -> const FPropertyValueSnapshotPayload& { return OriginalValue; }
		auto GetCurrentValue() const -> const FPropertyValueSnapshotPayload& { return CurrentValue; }

	private:
		auto BeginInternal(const FPropertyEditTarget& Target, std::string_view Description,
			DTransactor* Transactor, std::string* OutError) -> bool;
		friend class FPropertyView;
		friend class PropertyEditor::FPropertyEdit;
		auto ApplyInternal(const FPropertyValueSnapshotPayload& ProposedValue,
			std::string* OutError, bool* OutChanged) -> bool;
		auto Reset() -> void;

		FPropertyEditTarget Target;
		TStrongObjectPtr<DObject> TargetObject;
		FPropertyValueSnapshotPayload OriginalValue;
		FPropertyValueSnapshotPayload CurrentValue;
		std::string Description;
		DTransactor* Transactor = nullptr;
		std::optional<FScopedTransaction> TransactionScope;
		bool bActive = false;
	};
}
