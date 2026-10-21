#pragma once

#include "CoreMinimal.h"
#include "DObject/TransactionSerialization.h"
#include "DObject/TransactionObjectEvent.h"

namespace Durin
{
	struct FSerializedTaggedData
	{
		FName DeclaringType;
		FName Name;
		uint64 Begin = 0, End = 0;
		bool bReflected = false;
		auto operator==(const FSerializedTaggedData&) const -> bool = default;
	};

	// Immutable transfer allocations are shared with undo data. Tags and identity
	// belong to observation; annotations, loading and compensation do not.
	struct FDiffableObject
	{
		FSerializedObjectInfo ObjectInfo;
		std::shared_ptr<const FSerializedObject> SerializedObject;
		std::shared_ptr<const FTransactionSerializationContext> SerializationContext;
		std::vector<FSerializedTaggedData> SerializedTaggedData;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		COREDOBJECT_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
	};

	namespace DiffUtil
	{
		struct FGenerateObjectDiffOptions
		{
			bool bFullDiff = true;
			std::function<bool(FName)> ShouldSkipProperty;
		};
		COREDOBJECT_API auto PrepareObjectReplacement(const FDiffableObject& Source,
			const FObjectReplacementMap& Map) -> std::expected<FDiffableObject, std::string>;
		// Live ordinary objects only. Class-default override comparison is owned by
		// the default graph; pending-kill resurrection and arbitrary archetypes are
		// not inferred by the transaction observation path.
		COREDOBJECT_API auto GetDiffableObject(DObject& Object,
			const FSerializedObjectCapture* CapturePolicy = nullptr) -> std::expected<FDiffableObject, std::string>;
		COREDOBJECT_API auto GenerateObjectDiff(const FDiffableObject& OldObject, const FDiffableObject& NewObject,
			const FGenerateObjectDiffOptions& Options = {}) -> std::expected<FTransactionObjectChange, std::string>;
	}
}
