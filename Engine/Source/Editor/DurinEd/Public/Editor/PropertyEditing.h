#pragma once

#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
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
		std::function<bool(DObject&, FPropertyEditProposal&, std::string&)> PreEdit;
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
		std::vector<std::byte> MapKeyData;
		FPropertyValueSnapshotPayload MapKey;
	};

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
		std::vector<std::byte> LogicalIdentity;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;

		DURINED_API static auto ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex = 0) -> FPropertyEditTarget;
		DURINED_API auto ForStructMember(const FProperty* Property, uint32 ArrayIndex = 0) const -> FPropertyEditTarget;
		DURINED_API auto ForArrayElement(const FProperty* ElementProperty, uint64 ElementIndex) const -> FPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty, std::vector<std::byte> SerializedKey) const -> FPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty,
			FPropertyValueSnapshotPayload KeySnapshot,
			std::vector<std::byte> SerializedKey) const -> FPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty,
			const FPropertyValueSnapshot& KeySnapshot,
			std::vector<std::byte> SerializedKey) const -> FPropertyEditTarget;

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
			std::string* OutError = nullptr,
			DTransactor* InTransactor = nullptr
		) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError = nullptr) -> EPropertyEditResult;
		DURINED_API auto Apply(const FPropertyValueSnapshotPayload& ProposedValue,
			std::string* OutError = nullptr) -> EPropertyEditResult;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> EPropertyEditResult;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> EPropertyEditResult;

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
			bool bSucceeded,
			std::string Error,
			FPropertyValueSnapshotPayload ProposedValue) -> void;
		auto UpdateTransactorRecord(std::string* OutError) -> bool;
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
