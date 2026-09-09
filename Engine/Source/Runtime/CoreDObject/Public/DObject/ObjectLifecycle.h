#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"
#include "ObjectMacros.h"
#include "Misc/Name.h"

namespace Durin
{
	// Receives mutable object-reference slots during garbage-collector traversal.
	class FReferenceCollector
	{
	public:
		virtual ~FReferenceCollector() = default;
		virtual auto AddReferencedObject(DObject*& Object) -> void = 0;
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

	// Selects object flags that retain otherwise unreachable objects for one collection.
	struct FGarbageCollectionOptions
	{
		EObjectFlags KeepFlags = EObjectFlags::Standalone;
	};

	COREDOBJECT_API auto AddToRoot(DObject* Object) -> void;
	COREDOBJECT_API auto RemoveFromRoot(DObject* Object) -> void;
	COREDOBJECT_API auto IsValid(const DObject* Object) -> bool;
	COREDOBJECT_API auto MarkAsGarbage(DObject* Object) -> void;
	// Explicit structural teardown request without allocations or recursion, including
	// unpublished children. Does not make Outer a GC ownership edge or destroy objects.
	COREDOBJECT_API auto MarkObjectHierarchyAsGarbage(DObject* RootObject) -> void;
	// Clears every class owner derived-first and marks its template hierarchy for the host's object drain.
	COREDOBJECT_API auto ReleaseClassDefaultObjects() -> void;
	// Releases and drains only defaults owned by classes from one compiled-in module.
	COREDOBJECT_API auto ReleaseClassDefaultObjectsForModule(FName ModuleName) -> bool;
	COREDOBJECT_API auto ReleaseDStructDefaults() -> void;
	COREDOBJECT_API auto ReleaseDStructDefaultsForModule(FName ModuleName) -> void;
	// Defers physical collection throughout a game-thread execution region. Exit never collects.
	class FGarbageCollectionDeferralScope
	{
	public:
		COREDOBJECT_API FGarbageCollectionDeferralScope();
		COREDOBJECT_API ~FGarbageCollectionDeferralScope();
		FGarbageCollectionDeferralScope(const FGarbageCollectionDeferralScope&) = delete;
		auto operator=(const FGarbageCollectionDeferralScope&) -> FGarbageCollectionDeferralScope& = delete;
	};

	// Requests collection at a later safe point, including when automatic GC is disabled.
	COREDOBJECT_API auto RequestGarbageCollection(const FGarbageCollectionOptions& Options = {}) -> void;
	COREDOBJECT_API auto IsGarbageCollectionDeferred() -> bool;
	COREDOBJECT_API auto IsGarbageCollectionRequested() -> bool;
	// Collects at a safe point; inside a deferral region it records a request instead.
	COREDOBJECT_API auto CollectGarbage(
		const FGarbageCollectionOptions& Options = {}) -> void;
	COREDOBJECT_API auto GetGarbageObjectCount() -> uint64;
	COREDOBJECT_API auto GetLastGarbageCollectionStats() -> const FGarbageCollectionStats&;
	// Reports deferred objects and fails when the latest collection did not finish their destruction.
	COREDOBJECT_API auto CheckNoDeferredDestroyObjects(
		const char* Context) -> void;
	COREDOBJECT_API auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void;

	namespace Private
	{
		COREDOBJECT_API auto MarkTemplateObjectHierarchyAsGarbage(DObject* RootObject) -> void;
		// Native-test seam for validating one class-default teardown without releasing unrelated modules.
		COREDOBJECT_API auto ReleaseClassDefaultObjectForTests(DClass* Class) -> void;
	}
}
