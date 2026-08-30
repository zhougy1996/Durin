#pragma once

#include "DurinEdAPI.h"

#include "DObject/Archive.h"
#include "DObject/ObjectHandle.h"

namespace Durin
{
	class DObject;
	class FProperty;
	class FReferenceCollector;
	class FReflectedValueStorage;
}

namespace Durin::Editor
{
	// Identifies one exact object-array generation without retaining or path-resolving it.
	class FPersistentObjectRef
	{
	public:
		FPersistentObjectRef() = default;
		FPersistentObjectRef(std::nullptr_t) {}
		DURINED_API explicit FPersistentObjectRef(DObject* Object);
		DURINED_API static auto FromHandle(FObjectHandle Handle) -> FPersistentObjectRef;

		auto IsNull() const -> bool { return IsObjectHandleNull(Handle); }
		auto GetHandle() const -> FObjectHandle { return Handle; }
		DURINED_API auto Resolve() const -> DObject*;
		DURINED_API auto IsStale() const -> bool;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;

		friend auto operator==(
			const FPersistentObjectRef&, const FPersistentObjectRef&) -> bool = default;

	private:
		FObjectHandle Handle;
	};

	// Owns a deduplicated set of exact object identities for explicit collector traversal.
	class FTransactionObjectReferenceSet
	{
	public:
		DURINED_API auto Add(FPersistentObjectRef Reference) -> void;
		auto Num() const -> size_t { return References.size(); }
		auto GetReferences() const -> std::span<const FPersistentObjectRef>
		{
			return References;
		}
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;

	private:
		std::vector<FPersistentObjectRef> References;
	};

	// Locates one top-level reflected member and records the type expected by its payload.
	class FTransactionMemberLocator
	{
	public:
		DURINED_API static auto Capture(
			const FProperty* Property,
			uint32 ArrayIndex,
			FTransactionMemberLocator& OutLocator,
			std::string* OutError = nullptr) -> bool;
		DURINED_API auto Resolve(
			const DObject* Target,
			std::string* OutError = nullptr) const -> FProperty*;

		auto GetDeclaringType() const -> FName { return DeclaringType; }
		auto GetMemberName() const -> FName { return MemberName; }
		auto GetArrayIndex() const -> uint32 { return ArrayIndex; }

	private:
		FName DeclaringType;
		FName MemberName;
		uint32 ArrayIndex = 0;
		const FProperty* CapturedProperty = nullptr;
	};

	// Stores one participant's focused member state without retaining live storage addresses.
	class FFocusedTransactionObjectRecord
	{
	public:
		DURINED_API static auto Capture(
			DObject* Target,
			const FProperty* MemberProperty,
			uint32 ArrayIndex,
			FFocusedTransactionObjectRecord& OutRecord,
			std::string* OutError = nullptr) -> bool;

		auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
		auto GetMember() const -> const FTransactionMemberLocator& { return Member; }
		auto GetPayload() const -> const FPropertyValueSnapshotPayload& { return Payload; }
		auto GetHardReferences() const -> const FTransactionObjectReferenceSet&
		{
			return HardReferences;
		}

		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto RestoreDetached(
			FReflectedValueStorage& OutStorage,
			std::string* OutError = nullptr) const -> bool;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;

	private:
		FPersistentObjectRef Target;
		FTransactionMemberLocator Member;
		FPropertyValueSnapshotPayload Payload;
		FTransactionObjectReferenceSet HardReferences;
	};
}
