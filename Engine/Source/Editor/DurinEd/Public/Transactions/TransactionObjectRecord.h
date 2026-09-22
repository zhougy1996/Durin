#pragma once

#include <expected>

#include "DObject/PropertyChange.h"
#include "Transactions/TransactionRecord.h"

namespace Durin::Editor
{
	struct FPropertyEditTarget;

	struct FPropertyEditPathError;
	struct FPropertyValueDraftError;
	struct FPropertyMutationError;
	enum class ETransactionObjectRecordError : uint8
	{
		None, Target, SnapshotRoot, Payload, Member, Path, Draft, Mutation, Deferred
	};
	struct FTransactionObjectRecordError
	{
		ETransactionObjectRecordError Code = ETransactionObjectRecordError::None;
		FObjectKey Owner;
		std::string Member;
		std::string Snapshot;
		std::string Leaf;
		uint32 ArrayIndex = 0;
		uint64 PathLength = 0;
		std::string PathFirst;
		std::string PathLast;
		bool ObjectOwnedStorage = false;
		bool Before = false;
		bool BeforeValid = false;
		bool AfterValid = false;
		DurinCodeGen::EPropertyGenFlags ExpectedKind = DurinCodeGen::EPropertyGenFlags::None;
		DurinCodeGen::EPropertyGenFlags BeforeKind = DurinCodeGen::EPropertyGenFlags::None;
		DurinCodeGen::EPropertyGenFlags AfterKind = DurinCodeGen::EPropertyGenFlags::None;
		std::optional<FTransactionSnapshotError> MemberCause;
		std::shared_ptr<const FPropertyEditPathError> PathCause;
		std::shared_ptr<const FPropertyValueDraftError> DraftCause;
		std::shared_ptr<const FPropertyMutationError> MutationCause;
	};
	DURINED_API auto FormatTransactionObjectRecordError(const FTransactionObjectRecordError& Error) -> std::string;

	// Owns one stable member-to-leaf traversal step without retaining live storage.
	struct FTransactionPropertyPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		FByteBuffer MapKeyData;
		FPropertyValueSnapshotPayload MapKey;

		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
	};

	// Stores executable before/after property data while delegating application to the validated editor pipeline.
	class FTransactionObjectRecord
	{
	public:
		DURINED_API static auto Capture(
			const FPropertyEditTarget& Target,
			FPropertyValueSnapshotPayload Before,
			FPropertyValueSnapshotPayload After) -> std::expected<FTransactionObjectRecord, FTransactionObjectRecordError>;

		auto IsNoOp() const -> bool { return Before == After; }
		DURINED_API auto Validate() const -> std::expected<void, FTransactionObjectRecordError>;
		DURINED_API auto Apply(
			bool bBefore,
			EPropertyChangeOrigin Origin) const -> std::expected<void, FTransactionObjectRecordError>;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;

		auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
		auto GetBefore() const -> const FPropertyValueSnapshotPayload& { return Before; }
		auto GetAfter() const -> const FPropertyValueSnapshotPayload& { return After; }

	private:
		auto BuildTarget(FPropertyEditTarget& OutTarget) const -> std::expected<void, FTransactionObjectRecordError>;
		auto Reject(ETransactionObjectRecordError Code) const -> std::expected<void, FTransactionObjectRecordError>;

		FPersistentObjectRef Target;
		FTransactionMemberLocator SnapshotMember;
		const FProperty* LeafProperty = nullptr;
		std::vector<FTransactionPropertyPathSegment> Path;
		FByteBuffer LogicalIdentity;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;
		FPropertyValueSnapshotPayload Before;
		FPropertyValueSnapshotPayload After;
	};
}
