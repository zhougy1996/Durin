#include "DObject/TransactionObjectReference.h"

#include "DObject/Object.h"
#include "Misc/Transaction.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectGraphReplacement.h"

namespace Durin
{
	auto CanTransactObject(const DObject& Object) -> bool
	{
		if (!IsValid(&Object) || !Object.GetClass()) return false;
		if (!Object.IsTemplateObject()) return true;
		if (!Object.HasAnyObjectFlags(EObjectFlags::Transactional)) return false;
		const DObject* Root = &Object;
		while (Root->HasAnyObjectFlags(EObjectFlags::DefaultSubobject))
		{
			Root = Root->GetOuter();
			if (!IsValid(Root)) return false;
		}
		return Root->IsClassDefaultObject() && Root->GetClass()->GetDefaultObject() == Root;
	}

	FPersistentObjectRef::FPersistentObjectRef(DObject* Object)
		: Handle(FObjectKey(Object))
	{
		if (!IsValid(Object)) { Root = Handle; return; }
		DObject* RootObject = nullptr;
		for (auto* Current = Object; Current; Current = Current->GetOuter())
		{
			const bool bUseOuter = Current->HasAnyObjectFlags(EObjectFlags::DefaultSubobject)
				|| (Current->IsClassDefaultObject() && Current->GetClass()->GetDefaultObject() == Current);
			if (bUseOuter) RootObject = nullptr;
			else if (!RootObject) RootObject = Current;
		}
		Root = FObjectKey(RootObject);
		for (auto* Current = Object; Current && Current != RootObject; Current = Current->GetOuter())
			Hierarchy.push_back({Current->GetFName(), FObjectKey(Current->GetClass()), Current->IsClassDefaultObject(),
				Current->HasAnyObjectFlags(EObjectFlags::DefaultSubobject)});
		std::ranges::reverse(Hierarchy);
	}

	auto FPersistentObjectRef::FromKey(FObjectKey Handle) -> FPersistentObjectRef
	{
		FPersistentObjectRef Result;
		Result.Handle = Handle;
		Result.Root = Handle;
		return Result;
	}

	auto FPersistentObjectRef::GetHash() const noexcept -> size_t
	{
		size_t Hash = Root.GetHash();
		auto Combine = [&](size_t Part) { Hash ^= Part + 0x9e3779b9 + (Hash << 6) + (Hash >> 2); };
		for (const auto& Entry : Hierarchy)
		{
			Combine(std::hash<FName>{}(Entry.Name));
			Combine(Entry.Class.GetHash());
			Combine(Entry.bClassDefault);
			Combine(Entry.bDefaultSubobject);
		}
		return Hash;
	}

	auto FPersistentObjectRef::Resolve() const -> DObject*
	{
		DObject* Object = ResolveObjectKey(Root);
		if (!IsValid(Object)) return nullptr;
		for (const auto& Entry : Hierarchy)
		{
			DObject* Next = nullptr;
			for (auto* Child : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::IncludeTemplates))
				if (IsValid(Child) && Child->GetFName() == Entry.Name && FObjectKey(Child->GetClass()) == Entry.Class
					&& Child->IsClassDefaultObject() == Entry.bClassDefault
					&& Child->HasAnyObjectFlags(EObjectFlags::DefaultSubobject) == Entry.bDefaultSubobject)
				{
					if (Next) return nullptr;
					Next = Child;
				}
			if (!Next || (Entry.bClassDefault && Next->GetClass()->GetDefaultObject() != Next)) return nullptr;
			Object = Next;
		}
		return Object;
	}

	auto FPersistentObjectRef::Get() const -> DObject*
	{
		if (auto* Live = Resolve()) return Live;
		if (!Hierarchy.empty()) return nullptr;
		auto* Retained = GDObjectArray.Resolve(Root);
		return FScopedTransactionHistoryReplay::IncludesPendingKill(Retained) && !Retained->HasAnyInternalFlags(
			EObjectInternalFlags::BeginDestroyed | EObjectInternalFlags::FinishDestroyed) ? Retained : nullptr;
	}

	auto FPersistentObjectRef::CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FPersistentObjectRef, std::string>
	{
		if (IsNull()) return FPersistentObjectRef{};
		auto Key = !Hierarchy.empty() && !Map.Find(ResolveObjectKey(Handle))
			? Map.RemapObjectKey(FObjectKey(Resolve())) : Map.RemapObjectKey(Handle);
		if (!Key) return std::unexpected(ToString(Key.error()));
		if (IsObjectKeyNull(*Key)) return std::unexpected("A required transaction reference was removed.");
		auto* Object = ResolveObjectKey(*Key);
		if (Hierarchy.empty()) return FromKey(*Key);
		FPersistentObjectRef Result(Object);
		if (Result.Hierarchy.size() != Hierarchy.size()) return std::unexpected("Transaction subobject hierarchy changed.");
		for (size_t Index = 0; Index < Hierarchy.size(); ++Index)
		{
			auto Class = Map.RemapObjectKey(Hierarchy[Index].Class);
			if (!Class) return std::unexpected(ToString(Class.error()));
			auto Expected = Hierarchy[Index]; Expected.Class = *Class;
			if (Expected != Result.Hierarchy[Index]) return std::unexpected("Transaction subobject category or class changed.");
		}
		auto NewRoot = Map.RemapObjectKey(Root);
		if (!NewRoot || Result.Root != *NewRoot) return std::unexpected("Transaction subobject owner changed without a mapping.");
		return Result;
	}

	auto FPersistentObjectRef::IsStale() const -> bool
	{
		return !IsNull() && Resolve() == nullptr;
	}

	auto FPersistentObjectRef::AddReferencedObjects(
		FReferenceCollector& Collector) const -> void
	{
		// Never perform name lookup during collector traversal (UE's ARO contract).
		DObject* Object = IsGarbageCollecting() ? GDObjectArray.Resolve(Root) : ResolveObjectKey(Root);
		if (Object) Collector.AddReferencedObject(Object);
		// A logical subobject caches a weak leaf. Its root resolves the replacement
		// after reinstancing, so a retired cache entry must be allowed to expire.
		if (Handle != Root)
		{
			Object = ResolveObjectKey(Handle);
			if (Object) Collector.AddReferencedObject(Object);
		}
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
		for (const auto& Ref : References)
		{
			if (Ref.GetAllocatedSize() > std::numeric_limits<size_t>::max() - OutBytes) return false;
			OutBytes += Ref.GetAllocatedSize();
		}
		return true;
	}

}
