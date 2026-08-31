#pragma once

#include "DObject/PropertyChange.h"
#include "Editor/TransactionRecord.h"

namespace Durin::Editor
{
	struct FPropertyEditTarget;

	// Owns one stable member-to-leaf traversal step without retaining live storage.
	struct FTransactionPropertyPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		FByteArray MapKeyData;
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
			FPropertyValueSnapshotPayload After,
			FTransactionObjectRecord& OutRecord,
			std::string* OutError = nullptr) -> bool;

		auto IsNoOp() const -> bool { return Before == After; }
		DURINED_API auto Validate(std::string* OutError = nullptr) const -> bool;
		DURINED_API auto Apply(
			bool bBefore,
			EPropertyChangeOrigin Origin,
			std::string* OutError = nullptr) const -> bool;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;

		auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
		auto GetBefore() const -> const FPropertyValueSnapshotPayload& { return Before; }
		auto GetAfter() const -> const FPropertyValueSnapshotPayload& { return After; }

	private:
		auto BuildTarget(FPropertyEditTarget& OutTarget, std::string* OutError) const -> bool;

		FPersistentObjectRef Target;
		FTransactionMemberLocator SnapshotMember;
		const FProperty* LeafProperty = nullptr;
		std::vector<FTransactionPropertyPathSegment> Path;
		FByteArray LogicalIdentity;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;
		FPropertyValueSnapshotPayload Before;
		FPropertyValueSnapshotPayload After;
	};
}
