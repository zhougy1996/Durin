#pragma once

#include "CoreMinimal.h"

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

    // Native observers release external GC edges before irreversible object teardown.
    class FObjectReferencePurgeRegistration final
    {
    public:
        COREDOBJECT_API explicit FObjectReferencePurgeRegistration(std::function<void(DObject&)> Callback);
        COREDOBJECT_API ~FObjectReferencePurgeRegistration();
        FObjectReferencePurgeRegistration(const FObjectReferencePurgeRegistration&) = delete;
        auto operator=(const FObjectReferencePurgeRegistration&) -> FObjectReferencePurgeRegistration& = delete;
    private:
        uint64 Identity;
    };
    COREDOBJECT_API auto NotifyObjectReferencesInvalidated(DObject& Object) -> void;
	// Actual collection telemetry. Purge-only work updates destruction totals/timing,
	// but never advances the collection serial, frame, time, or object-count snapshot.
	struct FGarbageCollectionStats
	{
		uint64 CollectionSerial = 0;
		uint64 CollectionFrame = 0;
		double CollectionTime = 0.0;
		uint64 ObjectCountAfterCollection = 0;
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
	// Explicit structural teardown request without allocations or recursion, including
	// unpublished children. Does not make Outer a GC ownership edge or destroy objects.
	COREDOBJECT_API auto MarkObjectHierarchyAsGarbage(DObject* RootObject) -> void;
	// Clears every class owner derived-first and marks its template hierarchy for the host's object drain.
	COREDOBJECT_API auto ReleaseClassDefaultObjects() -> void;
	// Releases and drains only defaults owned by classes from one compiled-in module.
	COREDOBJECT_API auto ReleaseClassDefaultObjectsForModule(FName ModuleName) -> bool;
	COREDOBJECT_API auto ReleaseDStructDefaults() -> void;
	COREDOBJECT_API auto ReleaseDStructDefaultsForModule(FName ModuleName) -> void;
	// Requests collection at a later safe point, including when automatic GC is disabled.
	// Full-purge intent is sticky; retention flags belong exclusively to execution.
	COREDOBJECT_API auto RequestGarbageCollection(bool bForcePurge = false) -> void;
	COREDOBJECT_API auto IsGarbageCollecting() -> bool;
	COREDOBJECT_API auto IsGarbageCollectionRequested() -> bool;
	// Caller must establish a safe point, outside business callbacks and borrowed storage.
	// Attempts synchronous collection; collector reentry returns false and preserves requests.
	COREDOBJECT_API auto TryCollectGarbage(EObjectFlags KeepFlags, bool bPerformFullPurge = true) -> bool;
	// Synchronous execution at an owner-established safe point; collector reentry is diagnosed.
	// Business-stack safety is a caller precondition, not dynamically detected.
	// Finishes any previous purge first; false bounds only the new cycle's purge.
	COREDOBJECT_API auto CollectGarbage(
		EObjectFlags KeepFlags, bool bPerformFullPurge = true) -> void;
	// Advance only objects already found unreachable. Full drain waits for readiness;
	// callers must first release resources/fences requiring their own thread to progress.
	COREDOBJECT_API auto IncrementalPurgeGarbage(bool bUseTimeLimit = true, double TimeLimit = 0.002) -> void;
	COREDOBJECT_API auto IsIncrementalPurgePending() -> bool;
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
