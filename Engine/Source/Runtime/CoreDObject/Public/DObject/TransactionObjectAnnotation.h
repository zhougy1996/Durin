#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "DObject/TransactionObjectEvent.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class FArchive;
	class FReferenceCollector;
	class DObject;
	class FObjectReplacementMap;

	// Supplements serialized state with object-owned reconstruction information.
	// Captured annotations own their data and enumerate every retained hard edge.
	// Each capture must return an immutable snapshot. Transaction history is an
	// in-process owner; Serialize is for explicit annotation archive consumers,
	// not implicit persistence or part of the object's serialized payload.
	class ITransactionObjectAnnotation
	{
	public:
		virtual ~ITransactionObjectAnnotation() = default;
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) const -> void = 0;
		virtual auto Serialize(FArchive& Archive) -> void = 0;
		virtual auto GetAllocatedSize() const -> size_t = 0;
		// Preflight retained participants before any record loads historical fields.
		virtual auto Validate() const -> FTransactionCaptureResult { return {}; }
		// Refresh from live state while preserving any recovery participant scope
		// introduced by this snapshot. Ordinary annotations simply use the object hook.
		COREDOBJECT_API virtual auto CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation>;
		// Prepare an independent immutable snapshot; never rewrite a retained one.
		virtual auto CreateReplacement(const FObjectReplacementMap&) const
			-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>
		{ return std::unexpected("This annotation does not support object reinstancing."); }
		virtual auto ComputeAdditionalObjectChanges(const ITransactionObjectAnnotation* Original) const
			-> std::vector<std::pair<DObject*, FTransactionObjectChange>> { return {}; }
	};
	COREDOBJECT_API auto PrepareTransactionAnnotationReplacement(const std::shared_ptr<ITransactionObjectAnnotation>& Source,
		const FObjectReplacementMap& Map) -> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>;
}
