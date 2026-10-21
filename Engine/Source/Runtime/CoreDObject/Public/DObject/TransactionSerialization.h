#pragma once

#include "CoreMinimal.h"
#include "DObject/Archive.h"
#include "DObject/TransactionObjectReference.h"
#include "DObject/TransactionObjectId.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class FObjectReplacementMap;
	class IObjectReplacementParticipant;
	// Owned transaction bytes and persistent hard edges. Weak and soft values remain in
	// the byte stream and do not acquire collector ownership.
	struct FSerializedObject
	{
		FByteBuffer Bytes;
		std::vector<FPersistentObjectRef> HardReferences;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		auto operator==(const FSerializedObject&) const -> bool = default;
	};

	// Metadata captured alongside the payload, never a mutable identity registry.
	struct FSerializedObjectInfo
	{
		FPersistentObjectRef Target, Class, Outer, Package;
		FTransactionObjectId Identity;
		EObjectFlags Flags = EObjectFlags::NoFlags;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		auto operator==(const FSerializedObjectInfo& Other) const -> bool
		{
			return Target == Other.Target && Class == Other.Class && Outer == Other.Outer
				&& Package == Other.Package && Flags == Other.Flags && Identity.OriginalName == Other.Identity.OriginalName;
		}
	};

	struct FSerializedObjectReferenceSlot
	{
		uint64 Offset = 0;
		bool bWeak = false;
		auto operator==(const FSerializedObjectReferenceSlot&) const -> bool = default;
	};

	// Compatibility, filtering and reference layout belong to transfer, not annotations.
	struct FTransactionSerializationContext
	{
		FByteBuffer Schema;
		FArchiveVersionContext Versions;
		std::vector<FName> ExcludedMembers;
		std::vector<FSerializedObjectReferenceSlot> ReferenceSlots;
		COREDOBJECT_API auto IsCompatibleWith(const FTransactionSerializationContext& Other) const -> bool;
	};

	struct FDiffableObject;

	struct FSerializedObjectCapture
	{
		std::shared_ptr<const FSerializedObject> Data;
		FSerializedObjectInfo Info;
		std::shared_ptr<const FTransactionSerializationContext> Context;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		COREDOBJECT_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		// Replacement relevance includes stored weak slots without retaining them.
		COREDOBJECT_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;
		auto HasSameSchema(const FSerializedObjectCapture& Other) const -> bool
		{ return Info.Class == Other.Info.Class && Context && Other.Context && Context->IsCompatibleWith(*Other.Context); }
	};

	// These helpers never acquire or restore transaction annotations and never
	// compensate a failed load. The replay coordinator owns that failure policy.
	COREDOBJECT_API auto CaptureSerializedObject(DObject& Object,
		const FSerializedObjectCapture* CapturePolicy = nullptr, FDiffableObject* Observation = nullptr)
		-> std::expected<FSerializedObjectCapture, std::string>;
	COREDOBJECT_API auto ValidateSerializedObject(DObject& Object, const FSerializedObjectCapture& Capture)
		-> FTransactionCaptureResult;
	// Preflight first. Failure can follow partial writes; the caller owns recovery.
	COREDOBJECT_API auto LoadSerializedObject(DObject& Object, const FSerializedObjectCapture& Capture)
		-> FCommandChangeResult;
	// Prepare immutable copies only; callers publish through the graph participant protocol.
	COREDOBJECT_API auto PrepareSerializedObjectReplacement(const FSerializedObjectCapture& Source,
		const FObjectReplacementMap& Map) -> std::expected<FSerializedObjectCapture, std::string>;

	struct FSerializedObjectReplacementBinding
	{
		FSerializedObjectCapture* Capture = nullptr;
		FDiffableObject* Observation = nullptr;
	};
	// Owner must enumerate these captures/observations. Preparation rejects any
	// additional mapped native references rather than claiming partial coverage.
	COREDOBJECT_API auto CreateSerializedObjectReplacementParticipant(DObject& Owner,
		std::span<const FSerializedObjectReplacementBinding> Bindings) -> std::shared_ptr<IObjectReplacementParticipant>;

}
