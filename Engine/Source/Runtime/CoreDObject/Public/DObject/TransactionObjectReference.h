#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	class FReferenceCollector;
	class FObjectReplacementMap;

	// Native transaction admission does not grant reflected/script mutation access.
	COREDOBJECT_API auto CanTransactObject(const DObject& Object) -> bool;

	// UE root identity plus a bounded Outer/name chain for CDOs/default subobjects.
	// FromKey deliberately preserves exact generation semantics for opaque identities.
	class FPersistentObjectRef
	{
	public:
		enum class EReferenceType : uint8 { RootObject, SubObject };
		FPersistentObjectRef() = default;
		FPersistentObjectRef(std::nullptr_t) {}
		COREDOBJECT_API explicit FPersistentObjectRef(DObject* Object);
		COREDOBJECT_API static auto FromKey(FObjectKey Handle) -> FPersistentObjectRef;

		auto IsNull() const -> bool { return IsObjectKeyNull(Handle); }
		auto GetKey() const -> FObjectKey { return Handle; }
		auto GetReferenceType() const -> EReferenceType { return Hierarchy.empty() ? EReferenceType::RootObject : EReferenceType::SubObject; }
		auto GetAllocatedSize() const -> size_t { return Hierarchy.capacity() * sizeof(FSubObject); }
		COREDOBJECT_API auto CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FPersistentObjectRef, std::string>;
		COREDOBJECT_API auto Resolve() const -> DObject*;
		COREDOBJECT_API auto IsStale() const -> bool;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;

		friend auto operator==(const FPersistentObjectRef& A, const FPersistentObjectRef& B) -> bool
		{ return A.Root == B.Root && A.Hierarchy == B.Hierarchy; }

	private:
		FObjectKey Handle;
		FObjectKey Root;
		struct FSubObject
		{
			FName Name;
			FObjectKey Class;
			bool bClassDefault = false;
			bool bDefaultSubobject = false;
			auto operator==(const FSubObject&) const -> bool = default;
		};
		std::vector<FSubObject> Hierarchy;
	};

	// Owns deduplicated transaction references for explicit collector traversal.
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
