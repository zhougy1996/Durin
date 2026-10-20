#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	class FReferenceCollector;

	// Identifies one exact object-array generation without retaining or path-resolving it.
	class FPersistentObjectRef
	{
	public:
		FPersistentObjectRef() = default;
		FPersistentObjectRef(std::nullptr_t) {}
		COREDOBJECT_API explicit FPersistentObjectRef(DObject* Object);
		COREDOBJECT_API static auto FromKey(FObjectKey Handle) -> FPersistentObjectRef;

		auto IsNull() const -> bool { return IsObjectKeyNull(Handle); }
		auto GetKey() const -> FObjectKey { return Handle; }
		COREDOBJECT_API auto Resolve() const -> DObject*;
		COREDOBJECT_API auto IsStale() const -> bool;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;

		friend auto operator==(
			const FPersistentObjectRef&, const FPersistentObjectRef&) -> bool = default;

	private:
		FObjectKey Handle;
	};

	// Owns a deduplicated set of exact object identities for explicit collector traversal.
	class FTransactionObjectReferenceSet
	{
	public:
		COREDOBJECT_API auto Add(FPersistentObjectRef Reference) -> void;
		auto Num() const -> size_t { return References.size(); }
		auto GetReferences() const -> std::span<const FPersistentObjectRef>
		{
			return References;
		}
		COREDOBJECT_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;

	private:
		std::vector<FPersistentObjectRef> References;
	};

}
