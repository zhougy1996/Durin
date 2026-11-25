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
		enum class EGetDiffableObjectMode : uint8 { SerializeObject, SerializeProperties };
		struct FGetDiffableObjectOptions
		{
			EGetDiffableObjectMode ObjectSerializationMode = EGetDiffableObjectMode::SerializeObject;
		};
		struct FGenerateObjectDiffOptions
		{
			bool bFullDiff = true;
			std::function<bool(FName)> ShouldSkipProperty;
		};
		COREDOBJECT_API auto PrepareObjectReplacement(const FDiffableObject& Source,
			const FObjectReplacementMap& Map) -> std::expected<FDiffableObject, std::string>;
		// Observe a live object under its recorded serialization policy.
		COREDOBJECT_API auto GetDiffableObject(DObject& Object,
			const FSerializedObjectCapture* CapturePolicy = nullptr) -> std::expected<FDiffableObject, std::string>;
		// Properties-only observation includes generated references that ordinary
		// replay excludes. It is never an object transfer/replay payload.
		COREDOBJECT_API auto GetDiffableObject(DObject& Object, FGetDiffableObjectOptions Options)
			-> std::expected<FDiffableObject, std::string>;
		// An identity-only pending-kill observation compares restored data against the native template.
		COREDOBJECT_API auto GenerateObjectDiff(const FDiffableObject& OldObject, const FDiffableObject& NewObject,
			const FGenerateObjectDiffOptions& Options = {}) -> std::expected<FTransactionObjectChange, std::string>;
	}
}
