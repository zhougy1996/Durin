#include "DObject/TransactionObjectReference.h"

#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin
{
	FPersistentObjectRef::FPersistentObjectRef(DObject* Object)
		: Handle(FObjectKey(Object))
	{
	}

	auto FPersistentObjectRef::FromKey(FObjectKey Handle) -> FPersistentObjectRef
	{
		FPersistentObjectRef Result;
		Result.Handle = Handle;
		return Result;
	}

	auto FPersistentObjectRef::Resolve() const -> DObject*
	{
		DObject* Object = ResolveObjectKey(Handle);
		return IsValid(Object) ? Object : nullptr;
	}

	auto FPersistentObjectRef::IsStale() const -> bool
	{
		return !IsNull() && Resolve() == nullptr;
	}

	auto FPersistentObjectRef::AddReferencedObjects(
		FReferenceCollector& Collector) const -> void
	{
		DObject* Object = Resolve();
		if (Object) Collector.AddReferencedObject(Object);
	}

	auto FTransactionObjectReferenceSet::Add(FPersistentObjectRef Reference) -> void
	{
		if (Reference.IsNull()
			|| std::ranges::contains(References, Reference)) return;
		References.push_back(Reference);
	}

	auto FTransactionObjectReferenceSet::AddReferencedObjects(
		FReferenceCollector& Collector) const -> void
	{
		for (const FPersistentObjectRef& Reference : References)
			Reference.AddReferencedObjects(Collector);
	}

	auto FTransactionObjectReferenceSet::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		if (References.capacity() > std::numeric_limits<size_t>::max()
			/ sizeof(FPersistentObjectRef)) return false;
		OutBytes = References.capacity() * sizeof(FPersistentObjectRef);
		return true;
	}

}
