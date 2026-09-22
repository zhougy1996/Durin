#include "Transactions/TransactionRecord.h"

#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"

namespace Durin::Editor
{
	auto FormatTransactionSnapshotError(const FTransactionSnapshotError& Error) -> std::string
	{
		if (Error.SnapshotCause) return ToString(*Error.SnapshotCause);
		if (Error.ValueCause) return ToString(*Error.ValueCause);
		switch (Error.Code)
		{
		case ETransactionSnapshotError::None: return {};
		case ETransactionSnapshotError::NullMember: return "Cannot locate a null transaction member.";
		case ETransactionSnapshotError::MemberOwner: return "Focused transaction members must be top-level class properties.";
		case ETransactionSnapshotError::ArrayIndex: return "Focused transaction member array index is out of range.";
		case ETransactionSnapshotError::InvalidTarget: return "Focused transaction target no longer resolves.";
		case ETransactionSnapshotError::MissingMember: return "Focused transaction member no longer exists on the target class.";
		case ETransactionSnapshotError::IncompatibleMember: return "Focused transaction member is incompatible with the captured payload.";
		case ETransactionSnapshotError::MemberIdentity: return "Focused transaction member does not belong to the target class.";
		default: return "Focused transaction snapshot operation failed.";
		}
	}

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

	auto FTransactionMemberLocator::Capture(
		const FProperty* Property,
		uint32 InArrayIndex
	) -> std::expected<FTransactionMemberLocator, FTransactionSnapshotError>
	{
		FTransactionSnapshotError Error{.ArrayIndex = InArrayIndex};
		if (!Property) { Error.Code = ETransactionSnapshotError::NullMember; return std::unexpected(std::move(Error)); }
		Error.Member = Property->NamePrivate.ToString();
		Error.ArrayDim = Property->GetArrayDim();
		Error.ExpectedKind = Property->GetKind();
		const DClass* DeclaringClass = Cast<DClass>(Property->Owner.ToDObject());
		if (!DeclaringClass) { Error.Code = ETransactionSnapshotError::MemberOwner; return std::unexpected(std::move(Error)); }
		Error.DeclaringType = DeclaringClass->GetQualifiedName().ToString();
		if (InArrayIndex >= Property->GetArrayDim())
		{ Error.Code = ETransactionSnapshotError::ArrayIndex; return std::unexpected(std::move(Error)); }

		FTransactionMemberLocator Locator;
		Locator.DeclaringType = DeclaringClass->GetQualifiedName();
		Locator.MemberName = Property->NamePrivate;
		Locator.ArrayIndex = InArrayIndex;
		Locator.CapturedProperty = Property;
		return Locator;
	}

	auto FTransactionMemberLocator::Resolve(const DObject* Target) const -> std::expected<FProperty*, FTransactionSnapshotError>
	{
		FTransactionSnapshotError Error{
			.Owner = FObjectKey(Target), .Member = MemberName.ToString(),
			.DeclaringType = DeclaringType.ToString(), .ArrayIndex = ArrayIndex,
			.ExpectedKind = CapturedProperty ? CapturedProperty->GetKind() : DurinCodeGen::EPropertyGenFlags::None};
		if (!Target || !IsValid(Target))
		{ Error.Code = ETransactionSnapshotError::InvalidTarget; return std::unexpected(std::move(Error)); }
		DClass* TargetClass = Target->GetClass();
		FProperty* Property = TargetClass ? TargetClass->FindPropertyByName(MemberName) : nullptr;
		const DClass* DeclaringClass = Property ? Cast<DClass>(Property->Owner.ToDObject()) : nullptr;
		if (Property) { Error.ArrayDim = Property->GetArrayDim(); Error.ActualKind = Property->GetKind(); }
		if (DeclaringClass) Error.ActualDeclaringType = DeclaringClass->GetQualifiedName().ToString();
		if (!Property || !DeclaringClass || DeclaringClass->GetQualifiedName() != DeclaringType)
		{ Error.Code = ETransactionSnapshotError::MissingMember; return std::unexpected(std::move(Error)); }
		if (ArrayIndex >= Property->GetArrayDim())
		{ Error.Code = ETransactionSnapshotError::ArrayIndex; return std::unexpected(std::move(Error)); }
		if (!ArePropertySnapshotTypesCompatible(CapturedProperty, Property))
		{ Error.Code = ETransactionSnapshotError::IncompatibleMember; return std::unexpected(std::move(Error)); }
		return Property;
	}

	auto FFocusedTransactionObjectSnapshot::Capture(
		DObject* InTarget,
		const FProperty* MemberProperty,
		uint32 ArrayIndex
	) -> std::expected<FFocusedTransactionObjectSnapshot, FTransactionSnapshotError>
	{
		FTransactionSnapshotError Error{
			.Owner = FObjectKey(InTarget),
			.Member = MemberProperty ? MemberProperty->NamePrivate.ToString() : std::string{},
			.ArrayIndex = ArrayIndex};
		if (!IsValid(InTarget))
		{ Error.Code = ETransactionSnapshotError::InvalidTarget; return std::unexpected(std::move(Error)); }
		FFocusedTransactionObjectSnapshot Snapshot;
		Snapshot.Target = FPersistentObjectRef(InTarget);
		auto Member = FTransactionMemberLocator::Capture(MemberProperty, ArrayIndex);
		if (!Member)
		{ Member.error().Owner = Error.Owner; return std::unexpected(std::move(Member.error())); }
		Snapshot.Member = std::move(*Member);
		const auto Resolved = Snapshot.Member.Resolve(InTarget);
		if (!Resolved) return std::unexpected(Resolved.error());
		if (*Resolved != MemberProperty)
		{ Error.Code = ETransactionSnapshotError::MemberIdentity; return std::unexpected(std::move(Error)); }
		if (const auto Result = CapturePropertyValuePayload(MemberProperty, InTarget, ArrayIndex, Snapshot.Payload); !Result)
		{
			Error.Code = ETransactionSnapshotError::Capture;
			Error.SnapshotCause = Result.error();
			return std::unexpected(std::move(Error));
		}
		for (FObjectKey Handle : Snapshot.Payload.GetReferencedObjectKeys())
		{
			const FPersistentObjectRef Reference = FPersistentObjectRef::FromKey(Handle);
			if (Reference != Snapshot.Target) Snapshot.HardReferences.Add(Reference);
		}
		return Snapshot;
	}

	auto FFocusedTransactionObjectSnapshot::AddReferencedObjects(
		FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector);
		HardReferences.AddReferencedObjects(Collector);
	}

	auto FFocusedTransactionObjectSnapshot::RestoreDetached(
		FReflectedValueStorage& OutStorage
	) const -> std::expected<void, FTransactionSnapshotError>
	{
		const auto Resolved = Member.Resolve(Target.Resolve());
		if (!Resolved)
		{
			auto Error = Resolved.error();
			Error.Owner = Target.GetKey();
			return std::unexpected(std::move(Error));
		}
		FProperty* Property = *Resolved;
		FTransactionSnapshotError Error{
			.Owner = Target.GetKey(), .Member = Member.GetMemberName().ToString(),
			.DeclaringType = Member.GetDeclaringType().ToString(),
			.ArrayIndex = Member.GetArrayIndex(), .ArrayDim = Property->GetArrayDim(),
			.ExpectedKind = Property->GetKind(), .ActualKind = Property->GetKind()};
		FReflectedValueStorage Storage;
		if (const auto Result = Storage.DefaultConstruct(Property, Member.GetArrayIndex()); !Result)
		{ Error.Code = ETransactionSnapshotError::Storage; Error.ValueCause = Result.error(); return std::unexpected(std::move(Error)); }
		if (const auto Result = RestorePropertyValuePayload(Property, Storage.GetContainer(), Member.GetArrayIndex(), Payload); !Result)
		{ Error.Code = ETransactionSnapshotError::Restore; Error.SnapshotCause = Result.error(); return std::unexpected(std::move(Error)); }
		OutStorage = std::move(Storage);
		return {};
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
