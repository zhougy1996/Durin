#pragma once

#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
#include "DurinEdAPI.h"
#include "Editor/EditorTransaction.h"

namespace Durin
{
	class DObject;
	class DClass;
	class FProperty;

	struct FReflectedPropertyEditPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		std::vector<uint8> MapKeyData;
	};

	struct FReflectedPropertyEditTarget
	{
		DObject* Object = nullptr;
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		void* LeafContainer = nullptr;
		uint32 LeafArrayIndex = 0;
		// Nested container elements can move after an array resize or map rehash.
		// Transactions therefore snapshot a stable ancestor, normally the object-owned member.
		const FProperty* SnapshotProperty = nullptr;
		void* SnapshotContainer = nullptr;
		uint32 SnapshotArrayIndex = 0;
		std::vector<FReflectedPropertyEditPathSegment> Path;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;

		DURINED_API static auto ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex = 0) -> FReflectedPropertyEditTarget;
		DURINED_API auto ForStructMember(const FProperty* Property, void* StructContainer, uint32 ArrayIndex = 0) const -> FReflectedPropertyEditTarget;
		DURINED_API auto ForArrayElement(const FProperty* ElementProperty, void* ElementContainer, uint64 ElementIndex) const -> FReflectedPropertyEditTarget;
		DURINED_API auto ForMapEntry(const FProperty* EntryProperty, void* EntryContainer, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget;
	};

	class DURINED_API IReflectedPropertyMutationAdapter
	{
	public:
		virtual ~IReflectedPropertyMutationAdapter() = default;
		virtual auto Capture(const FReflectedPropertyEditTarget& Target, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool = 0;
		virtual auto Apply(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& ProposedValue, std::string* OutError) const -> bool = 0;
		virtual auto Restore(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool = 0;
	};

	DURINED_API auto GetGenericReflectedPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&;
	// Registered adapters are process-lifetime editor services because committed
	// transactions retain their selected adapter for future undo and redo.
	DURINED_API auto RegisterReflectedPropertyMutationAdapter(
		const DClass* ObjectClass,
		FName PropertyName,
		std::unique_ptr<IReflectedPropertyMutationAdapter> Adapter
	) -> bool;
	DURINED_API auto GetReflectedPropertyMutationAdapter(
		const FReflectedPropertyEditTarget& Target
	) -> const IReflectedPropertyMutationAdapter&;

	class FReflectedPropertyTransaction final : public IEditorTransaction
	{
	public:
		DURINED_API FReflectedPropertyTransaction(
			FReflectedPropertyEditTarget InTarget,
			FPropertyValueSnapshot InBefore,
			FPropertyValueSnapshot InAfter,
			std::string InDescription,
			const IReflectedPropertyMutationAdapter* InAdapter = nullptr
		);
		DURINED_API ~FReflectedPropertyTransaction() override;
		FReflectedPropertyTransaction(const FReflectedPropertyTransaction&) = delete;
		auto operator=(const FReflectedPropertyTransaction&) -> FReflectedPropertyTransaction& = delete;

		auto GetDescription() const -> std::string_view override { return Description; }
		DURINED_API auto GetDetails(EEditorTransactionOperation Operation) const -> std::string override;
		DURINED_API auto Undo() -> bool override;
		DURINED_API auto Redo() -> bool override;

	private:
		auto Restore(const FPropertyValueSnapshot& Snapshot, EPropertyChangeOrigin Origin) -> bool;
		auto Notify(EPropertyChangeOrigin Origin) const -> void;

		FReflectedPropertyEditTarget Target;
		FPropertyValueSnapshot Before;
		FPropertyValueSnapshot After;
		std::string Description;
		std::string LastError;
		const IReflectedPropertyMutationAdapter* Adapter = nullptr;
		bool bObjectRooted = false;
	};

	enum class EReflectedPropertyEditResult : uint8
	{
		Failed,
		NoChange,
		Changed,
	};

	class FReflectedPropertyEditSession
	{
	public:
		DURINED_API ~FReflectedPropertyEditSession();
		FReflectedPropertyEditSession() = default;
		FReflectedPropertyEditSession(const FReflectedPropertyEditSession&) = delete;
		auto operator=(const FReflectedPropertyEditSession&) -> FReflectedPropertyEditSession& = delete;

		DURINED_API auto Begin(
			const FReflectedPropertyEditTarget& InTarget,
			std::string_view InDescription,
			// Custom adapters are registry-owned and must outlive the session and its committed transaction.
			const IReflectedPropertyMutationAdapter* InAdapter = nullptr,
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
		auto Notify(EPropertyChangePhase Phase) const -> void;
		auto Reset() -> void;

		FReflectedPropertyEditTarget Target;
		const IReflectedPropertyMutationAdapter* Adapter = nullptr;
		FPropertyValueSnapshot OriginalValue;
		FPropertyValueSnapshot CurrentValue;
		std::string Description;
		FEditorTransactionManager* TransactionManager = nullptr;
		bool bActive = false;
		bool bObjectRooted = false;
	};
}
