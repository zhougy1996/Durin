#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"

namespace Durin
{
	// Receives mutable object-reference slots during garbage-collector traversal.
	class FReferenceCollector
	{
	public:
		virtual ~FReferenceCollector() = default;
		virtual auto AddReferencedObject(DObject*& Object) -> void = 0;
	};

	// Keeps one object rooted for the lifetime of this movable scope guard.
	class FScopedObjectRoot
	{
	public:
		COREDOBJECT_API explicit FScopedObjectRoot(DObject* InObject);
		COREDOBJECT_API ~FScopedObjectRoot();
		FScopedObjectRoot(const FScopedObjectRoot&) = delete;
		auto operator=(const FScopedObjectRoot&) -> FScopedObjectRoot& = delete;
		COREDOBJECT_API FScopedObjectRoot(FScopedObjectRoot&& Other) noexcept;
		COREDOBJECT_API auto operator=(FScopedObjectRoot&& Other) noexcept -> FScopedObjectRoot&;

	private:
		DObject* Object = nullptr;
	};

	// Summarizes the most recent mark-and-sweep collection and its phase timings.
	struct FGarbageCollectionStats
	{
		uint64 MarkedObjectCount = 0;
		uint64 CandidateObjectCount = 0;
		uint64 SweptObjectCount = 0;
		uint64 DeferredDestroyObjectCount = 0;
		double MarkMilliseconds = 0.0;
		double SweepMilliseconds = 0.0;
	};

	COREDOBJECT_API auto AddToRoot(DObject* Object) -> void;
	COREDOBJECT_API auto RemoveFromRoot(DObject* Object) -> void;
	COREDOBJECT_API auto IsValid(const DObject* Object) -> bool;
	COREDOBJECT_API auto MarkAsGarbage(DObject* Object) -> void;
	// Explicit structural teardown request; this does not make Outer a GC ownership edge.
	COREDOBJECT_API auto MarkObjectHierarchyAsGarbage(DObject* RootObject) -> void;
	COREDOBJECT_API auto CollectGarbage() -> void;
	COREDOBJECT_API auto GetGarbageObjectCount() -> uint64;
	COREDOBJECT_API auto GetLastGarbageCollectionStats() -> const FGarbageCollectionStats&;
	// Reports deferred objects and fails when the latest collection did not finish their destruction.
	COREDOBJECT_API auto CheckNoDeferredDestroyObjects(
		const char* Context) -> void;
	COREDOBJECT_API auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void;
}
