#pragma once

#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
#include "DurinEdAPI.h"
#include "Editor/EditorTransaction.h"

namespace Durin
{
	class DObject;
	class FProperty;

	// Identifies one stable traversal step from a reflected member to a nested value.
	struct FReflectedPropertyEditPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		std::vector<uint8> MapKeyData;
		FPropertyValueSnapshot MapKey;
	};

	// Describes a reflected edit using a stable snapshot root and logical path.
	struct FReflectedPropertyEditTarget
	{
		DObject* Object = nullptr;
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		// Nested container elements can move after an array resize or map rehash.
		// Transactions therefore snapshot a stable ancestor, normally the object-owned member.
		const FProperty* SnapshotProperty = nullptr;
		void* SnapshotContainer = nullptr;
		uint32 SnapshotArrayIndex = 0;
		std::vector<FReflectedPropertyEditPathSegment> Path;
		// Logical identity distinguishes independently edited values that intentionally
		// share one stable snapshot root, such as GUID-addressed array entries.
		std::vector<uint8> LogicalIdentity;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;

		DURINED_API static auto ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex = 0) -> FReflectedPropertyEditTarget;
		DURINED_API auto ForStructMember(const FProperty* Property, uint32 ArrayIndex = 0) const -> FReflectedPropertyEditTarget;
		DURINED_API auto ForArrayElement(const FProperty* ElementProperty, uint64 ElementIndex) const -> FReflectedPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty,
			FPropertyValueSnapshot KeySnapshot, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget;
	};

	// Restores before/after snapshots for one committed reflected-property edit.
	class FReflectedPropertyTransaction final : public IEditorTransaction
	{
	public:
		DURINED_API FReflectedPropertyTransaction(
			FReflectedPropertyEditTarget InTarget,
			FPropertyValueSnapshot InBefore,
			FPropertyValueSnapshot InAfter,
			std::string InDescription
		);
		DURINED_API ~FReflectedPropertyTransaction() override;
		FReflectedPropertyTransaction(const FReflectedPropertyTransaction&) = delete;
		auto operator=(const FReflectedPropertyTransaction&) -> FReflectedPropertyTransaction& = delete;

		auto GetDescription() const -> std::string_view override { return Description; }
		DURINED_API auto GetDetails(EEditorTransactionOperation Operation) const -> std::string override;
		auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
		DURINED_API auto Undo() -> bool override;
		DURINED_API auto Redo() -> bool override;

	private:
		auto Restore(const FPropertyValueSnapshot& Snapshot, EPropertyChangeOrigin Origin) -> bool;

		FReflectedPropertyEditTarget Target;
		FPropertyValueSnapshot Before;
		FPropertyValueSnapshot After;
		std::string Description;
		std::string LastError;
		std::array<DPackage*, 1> AffectedPackages{};
		bool bObjectRooted = false;
	};

	// Reports whether a reflected edit failed, changed nothing, or changed value.
	enum class EReflectedPropertyEditResult : uint8
	{
		Failed,
		NoChange,
		Changed,
	};

	// Coalesces continuous widget changes into one reflected-property transaction.
	class FReflectedPropertyEditSession
	{
	public:
		DURINED_API ~FReflectedPropertyEditSession();
		FReflectedPropertyEditSession() = default;
		FReflectedPropertyEditSession(const FReflectedPropertyEditSession&) = delete;
		auto operator=(const FReflectedPropertyEditSession&) -> FReflectedPropertyEditSession& = delete;

		DURINED_API auto Begin(
			const FReflectedPropertyEditTarget& InTarget,
			// An empty description uses "Edit <MemberProperty>" after target validation.
			std::string_view InDescription,
			std::string* OutError = nullptr,
			FEditorTransactionManager* InTransactionManager = nullptr
		) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError = nullptr) -> EReflectedPropertyEditResult;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> EReflectedPropertyEditResult;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> EReflectedPropertyEditResult;

		auto IsActive() const -> bool { return bActive; }
		DURINED_API auto MatchesTarget(const FReflectedPropertyEditTarget& Other) const -> bool;
		auto HasChanges() const -> bool { return bActive && !(OriginalValue == CurrentValue); }
		auto GetDescription() const -> std::string_view { return Description; }
		auto GetOriginalValue() const -> const FPropertyValueSnapshot& { return OriginalValue; }
		auto GetCurrentValue() const -> const FPropertyValueSnapshot& { return CurrentValue; }

	private:
		auto Reset() -> void;

		FReflectedPropertyEditTarget Target;
		FPropertyValueSnapshot OriginalValue;
		FPropertyValueSnapshot CurrentValue;
		std::string Description;
		FEditorTransactionManager* TransactionManager = nullptr;
		bool bActive = false;
		bool bObjectRooted = false;
	};
}
