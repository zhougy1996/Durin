#include "Editor/TransactionRecord.h"

#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"

namespace Durin::Editor
{
	namespace
	{
		auto Fail(std::string* OutError, std::string_view Error) -> bool
		{
			if (OutError) *OutError = Error;
			return false;
		}
	}

	FPersistentObjectRef::FPersistentObjectRef(DObject* Object)
		: Handle(MakeObjectHandle(Object))
	{
	}

	auto FPersistentObjectRef::FromHandle(FObjectHandle Handle) -> FPersistentObjectRef
	{
		FPersistentObjectRef Result;
		Result.Handle = Handle;
		return Result;
	}

	auto FPersistentObjectRef::Resolve() const -> DObject*
	{
		DObject* Object = ResolveObjectHandle(Handle);
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
			|| std::ranges::find(References, Reference) != References.end()) return;
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

	auto FTransactionMemberLocator::Capture(
		const FProperty* Property,
		uint32 InArrayIndex,
		FTransactionMemberLocator& OutLocator,
		std::string* OutError
	) -> bool
	{
		if (OutError) OutError->clear();
		if (!Property) return Fail(OutError, "Cannot locate a null transaction member.");
		const DClass* DeclaringClass = Cast<DClass>(Property->Owner.ToDObject());
		if (!DeclaringClass)
			return Fail(OutError, "Focused transaction members must be top-level class properties.");
		if (InArrayIndex >= Property->GetArrayDim())
			return Fail(OutError, "Focused transaction member array index is out of range.");

		FTransactionMemberLocator Locator;
		Locator.DeclaringType = DeclaringClass->GetQualifiedName();
		Locator.MemberName = Property->NamePrivate;
		Locator.ArrayIndex = InArrayIndex;
		Locator.CapturedProperty = Property;
		OutLocator = Locator;
		return true;
	}

	auto FTransactionMemberLocator::Resolve(
		const DObject* Target,
		std::string* OutError
	) const -> FProperty*
	{
		if (OutError) OutError->clear();
		if (!Target || !IsValid(Target))
		{
			Fail(OutError, "Focused transaction target no longer resolves.");
			return nullptr;
		}
		DClass* TargetClass = Target->GetClass();
		FProperty* Property = TargetClass
			? TargetClass->FindPropertyByName(MemberName) : nullptr;
		const DClass* DeclaringClass = Property
			? Cast<DClass>(Property->Owner.ToDObject()) : nullptr;
		if (!Property || !DeclaringClass
			|| DeclaringClass->GetQualifiedName() != DeclaringType)
		{
			Fail(OutError, "Focused transaction member no longer exists on the target class.");
			return nullptr;
		}
		if (ArrayIndex >= Property->GetArrayDim()
			|| !ArePropertySnapshotTypesCompatible(CapturedProperty, Property))
		{
			Fail(OutError, "Focused transaction member is incompatible with the captured payload.");
			return nullptr;
		}
		return Property;
	}

	auto FFocusedTransactionObjectSnapshot::Capture(
		DObject* InTarget,
		const FProperty* MemberProperty,
		uint32 ArrayIndex,
		FFocusedTransactionObjectSnapshot& OutSnapshot,
		std::string* OutError
	) -> bool
	{
		if (OutError) OutError->clear();
		if (!IsValid(InTarget))
			return Fail(OutError, "Cannot capture a focused record for an invalid target.");

		FFocusedTransactionObjectSnapshot Snapshot;
		Snapshot.Target = FPersistentObjectRef(InTarget);
		if (!FTransactionMemberLocator::Capture(
			MemberProperty, ArrayIndex, Snapshot.Member, OutError)) return false;
		if (Snapshot.Member.Resolve(InTarget, OutError) != MemberProperty)
			return Fail(OutError, "Focused transaction member does not belong to the target class.");
		if (!CapturePropertyValuePayload(
			MemberProperty, InTarget, ArrayIndex, Snapshot.Payload, OutError)) return false;

		for (FObjectHandle Handle : Snapshot.Payload.GetReferencedObjectHandles())
		{
			const FPersistentObjectRef Reference = FPersistentObjectRef::FromHandle(Handle);
			if (Reference != Snapshot.Target) Snapshot.HardReferences.Add(Reference);
		}
		OutSnapshot = std::move(Snapshot);
		return true;
	}

	auto FFocusedTransactionObjectSnapshot::AddReferencedObjects(
		FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector);
		HardReferences.AddReferencedObjects(Collector);
	}

	auto FFocusedTransactionObjectSnapshot::RestoreDetached(
		FReflectedValueStorage& OutStorage,
		std::string* OutError
	) const -> bool
	{
		if (OutError) OutError->clear();
		DObject* ResolvedTarget = Target.Resolve();
		FProperty* Property = Member.Resolve(ResolvedTarget, OutError);
		if (!Property) return false;

		FReflectedValueStorage Storage;
		if (!Storage.DefaultConstruct(Property, Member.GetArrayIndex(), OutError)) return false;
		if (!RestorePropertyValuePayload(
			Property, Storage.GetContainer(), Member.GetArrayIndex(), Payload, OutError))
		{
			return false;
		}
		OutStorage = std::move(Storage);
		return true;
	}

	auto FFocusedTransactionObjectSnapshot::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		size_t PayloadBytes = 0;
		size_t ReferenceBytes = 0;
		if (!Payload.TryGetAllocatedSize(PayloadBytes)
			|| !HardReferences.TryGetAllocatedSize(ReferenceBytes)
			|| PayloadBytes > std::numeric_limits<size_t>::max() - ReferenceBytes)
		{
			return false;
		}
		OutBytes = PayloadBytes + ReferenceBytes;
		return true;
	}
}
