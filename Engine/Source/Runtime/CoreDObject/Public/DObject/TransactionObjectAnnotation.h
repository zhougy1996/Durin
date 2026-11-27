#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class FReferenceCollector;
	class DObject;
	class FObjectReplacementMap;

	// Supplements serialized state with object-owned reconstruction information.
	// Captured annotations own their data and enumerate every retained hard edge.
	// Captured values stay fixed; reinstancing can update their references. Transaction history owns them in process.
	class ITransactionObjectAnnotation
	{
	public:
		virtual ~ITransactionObjectAnnotation() = default;
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) -> void = 0;
		virtual auto GetAllocatedSize() const -> size_t = 0;
		// Allows native records to opt into the same expiry policy as custom changes.
		virtual auto HasExpired() const noexcept -> bool { return false; }
		// Preflight retained participants before any record loads historical fields.
		virtual auto Validate() const -> FTransactionCaptureResult { return {}; }
		// Refresh from live state while preserving any recovery participant scope
		// introduced by this snapshot. Ordinary annotations simply use the object hook.
		COREDOBJECT_API virtual auto CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation>;
		// Typed reconstruction caches also rebind encoded component identities.
		COREDOBJECT_API virtual auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	};
	COREDOBJECT_API auto ReinstanceTransactionAnnotation(const std::shared_ptr<ITransactionObjectAnnotation>& Source,
		const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
}
