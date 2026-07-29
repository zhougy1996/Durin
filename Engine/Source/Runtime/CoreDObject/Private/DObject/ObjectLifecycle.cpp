#include "DObject/ObjectLifecycle.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/GarbageCollectionScheduler.h"
#include "DObject/Object.h"
#include "CoreGlobals.h"
#include "Misc/Time.h"
#include "Threading/RunnableThread.h"
#include "GCReferenceSchema.h"

namespace Durin
{
	namespace
	{
		FGarbageCollectionStats GLastGarbageCollectionStats;

		auto CheckObjectThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto IsPermanentObject(DObject* Object) -> bool
		{
			if (!Object) return true;
			if (EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Intrinsic)) return true;
			DClass* ObjectClass = Object->GetClass();
			return ObjectClass && DType::StaticClass() && Object->IsA(DType::StaticClass());
		}

		auto MarkGarbageInternal(DObject* Object) -> void
		{
			if (!Object || !GDObjectArray.Contains(Object) || IsPermanentObject(Object) || Object->IsGarbage()) return;
			Object->SetInternalFlags(EObjectInternalFlags::Garbage);
			GDObjectArray.NotifyObjectMarkedGarbage();
		}

		auto GatherDestroyOrder(std::span<DObject* const> Candidates) -> std::vector<DObject*>
		{
			struct FStackEntry
			{
				DObject* Object;
				bool bExpanded;
			};

			std::vector<DObject*> Order;
			std::vector<FStackEntry> Stack;
			const std::unordered_set<DObject*> CandidateSet(Candidates.begin(), Candidates.end());
			std::unordered_set<DObject*> Added;
			Stack.reserve(Candidates.size());
			for (DObject* Candidate : Candidates) Stack.push_back({Candidate, false});

			while (!Stack.empty())
			{
				const FStackEntry Entry = Stack.back();
				Stack.pop_back();
				DObject* Object = Entry.Object;
				if (!Object || !CandidateSet.contains(Object) || !GDObjectArray.Contains(Object) || IsPermanentObject(Object)) continue;

				if (Entry.bExpanded)
				{
					Order.push_back(Object);
					continue;
				}
				if (!Added.insert(Object).second) continue;

				Stack.push_back({Object, true});
				for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object, true))
				{
					if (CandidateSet.contains(Inner)) Stack.push_back({Inner, false});
				}
			}
			return Order;
		}

		// Physical destruction is deliberately narrower than a public destruction
		// request: GC must complete every lifecycle phase before reaching this point.
		auto DestroyObject(DObject* Object) -> void
		{
			check(Object);
			check(GDObjectArray.Contains(Object));
			check(Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed));
			check(Object->IsReadyForFinishDestroy());
			check(Object->HasAnyInternalFlags(EObjectInternalFlags::FinishDestroyed));

			// A forcibly garbage Outer may still have a reachable child. Removing the
			// Outer detaches such children instead of treating hierarchy as ownership.
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Object, true))
			{
				Child->SetOuterPrivate(nullptr);
			}
			Object->SetOuterPrivate(nullptr);
			GDObjectArray.Remove(Object);
			delete Object;
		}

		class FMarkReferenceCollector : public FReferenceCollector
		{
		public:
			auto AddReferencedObject(DObject*& Object) -> void override { Enqueue(Object); }
			auto Enqueue(DObject* Object) -> void
			{
				if (Object) Pending.push_back(Object);
			}

			auto Drain() -> uint64
			{
				uint64 MarkedCount = 0;
				while (!Pending.empty())
				{
					DObject* Object = Pending.back();
					Pending.pop_back();
					if (!Object || !GDObjectArray.Contains(Object) || Object->IsGarbage()
						|| Object->HasAnyInternalFlags(EObjectInternalFlags::Reachable)) continue;

					Object->SetInternalFlags(EObjectInternalFlags::Reachable);
					++MarkedCount;
					// Outer is a one-way lifetime reference: a reachable child keeps its
					// hierarchy alive, but the hierarchy never owns or keeps children alive.
					Enqueue(Object->GetOuter());
					Object->AddReferencedObjects(*this);
				}
				return MarkedCount;
			}

		private:
			std::vector<DObject*> Pending;
		};
	}

	FScopedObjectRoot::FScopedObjectRoot(DObject* InObject)
		: Object(InObject)
	{
		AddToRoot(Object);
	}

	FScopedObjectRoot::~FScopedObjectRoot()
	{
		RemoveFromRoot(Object);
	}

	FScopedObjectRoot::FScopedObjectRoot(FScopedObjectRoot&& Other) noexcept
		: Object(Other.Object)
	{
		Other.Object = nullptr;
	}

	auto FScopedObjectRoot::operator=(FScopedObjectRoot&& Other) noexcept -> FScopedObjectRoot&
	{
		if (this == &Other) return *this;
		RemoveFromRoot(Object);
		Object = Other.Object;
		Other.Object = nullptr;
		return *this;
	}

	auto AddToRoot(DObject* Object) -> void
	{
		CheckObjectThread();
		if (!Object) return;
		check(GDObjectArray.Contains(Object));
		++Object->RootReferenceCount;
		Object->SetInternalFlags(EObjectInternalFlags::RootSet);
	}

	auto RemoveFromRoot(DObject* Object) -> void
	{
		CheckObjectThread();
		if (!Object) return;
		check(GDObjectArray.Contains(Object));
		check(Object->RootReferenceCount > 0);
		if (--Object->RootReferenceCount == 0) Object->ClearInternalFlags(EObjectInternalFlags::RootSet);
	}

	auto IsValid(const DObject* Object) -> bool
	{
		return Object && GDObjectArray.Contains(Object) && !Object->IsPendingKill();
	}

	auto MarkAsGarbage(DObject* Object) -> void
	{
		CheckObjectThread();
		MarkGarbageInternal(Object);
	}

	auto MarkObjectHierarchyAsGarbage(DObject* RootObject) -> void
	{
		CheckObjectThread();
		if (!RootObject || !GDObjectArray.Contains(RootObject)) return;

		std::vector<DObject*> Pending = {RootObject};
		while (!Pending.empty())
		{
			DObject* Object = Pending.back();
			Pending.pop_back();
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Object, true)) Pending.push_back(Child);
			MarkGarbageInternal(Object);
		}
	}

	COREDOBJECT_API auto ConditionallyMarkAsReachable(DObject* Object) -> void
	{
		(void)Object;
	}

	auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void
	{
		if (!Object || !Object->GetClass()) return;
		Private::FGCReferenceSchemaRegistry::Visit(Object->GetClass(), Object, Collector);
	}

	auto CollectGarbage() -> void
	{
		CheckObjectThread();
		GLastGarbageCollectionStats = {};
		const double MarkStartTime = FTime::Seconds();
		for (DObject* Object : GDObjectArray.GetAll()) Object->ClearInternalFlags(EObjectInternalFlags::Reachable);

		FMarkReferenceCollector Marker;
		for (DObject* Object : GDObjectArray.GetAll())
		{
			if (!Object->IsGarbage() && (Object->HasAnyInternalFlags(EObjectInternalFlags::RootSet) || IsPermanentObject(Object)))
			{
				Marker.Enqueue(Object);
			}
		}
		GLastGarbageCollectionStats.MarkedObjectCount = Marker.Drain();
		GLastGarbageCollectionStats.MarkMilliseconds = (FTime::Seconds() - MarkStartTime) * 1000.0;

		std::vector<DObject*> SweepCandidates;
		for (DObject* Object : GDObjectArray.GetAll())
		{
			if (!IsPermanentObject(Object) && (Object->IsGarbage() || !Object->HasAnyInternalFlags(EObjectInternalFlags::Reachable)))
			{
				SweepCandidates.push_back(Object);
			}
		}
		GLastGarbageCollectionStats.CandidateObjectCount = static_cast<uint64>(SweepCandidates.size());

		const double SweepStartTime = FTime::Seconds();
		std::vector<DObject*> DestroyOrder = GatherDestroyOrder(SweepCandidates);
		for (DObject* Object : DestroyOrder) MarkGarbageInternal(Object);
		for (auto It = DestroyOrder.rbegin(); It != DestroyOrder.rend(); ++It)
		{
			DObject* Object = *It;
			if (Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed)) continue;
			Object->SetInternalFlags(EObjectInternalFlags::BeginDestroyed);
			Object->BeginDestroy();
		}

		for (DObject* Object : DestroyOrder)
		{
			if (Object->HasAnyInternalFlags(EObjectInternalFlags::FinishDestroyed)
				|| !Object->IsReadyForFinishDestroy()) continue;
			Object->FinishDestroy();
			Object->SetInternalFlags(EObjectInternalFlags::FinishDestroyed);
		}

		uint64 DestroyedObjectCount = 0;
		for (DObject* Object : DestroyOrder)
		{
			if (!Object->HasAnyInternalFlags(EObjectInternalFlags::FinishDestroyed)) continue;
			DestroyObject(Object);
			++DestroyedObjectCount;
		}
		GLastGarbageCollectionStats.SweptObjectCount = DestroyedObjectCount;
		GLastGarbageCollectionStats.DeferredDestroyObjectCount =
			static_cast<uint64>(DestroyOrder.size()) - DestroyedObjectCount;
		GLastGarbageCollectionStats.SweepMilliseconds = (FTime::Seconds() - SweepStartTime) * 1000.0;
		NotifyGarbageCollectionCompleted(FTime::Seconds(), GLastGarbageCollectionStats);
	}

	auto GetGarbageObjectCount() -> uint64
	{
		return GDObjectArray.GetGarbageNum();
	}

	auto GetLastGarbageCollectionStats() -> const FGarbageCollectionStats&
	{
		return GLastGarbageCollectionStats;
	}

	auto CheckNoDeferredDestroyObjects(const char* Context) -> void
	{
		if (GLastGarbageCollectionStats.DeferredDestroyObjectCount == 0)
		{
			return;
		}

		for (const DObject* Object : GDObjectArray.GetAll())
		{
			if (!Object
				|| !Object->HasAnyInternalFlags(
					EObjectInternalFlags::BeginDestroyed)
				|| Object->HasAnyInternalFlags(
					EObjectInternalFlags::FinishDestroyed))
			{
				continue;
			}
			DURIN_ERROR_CATEGORY(
				"GC",
				"Object remained deferred during '{}': path='{}', class='{}', "
				"internal_flags={}.",
				Context,
				Object->GetObjectPath(),
				Object->GetClass()
					? Object->GetClass()->GetQualifiedName().ToString()
					: "<unregistered>",
				static_cast<uint32>(Object->GetInternalFlags()));
		}

		checkf(
			false,
			"{} left {} deferred object(s).",
			Context,
			GLastGarbageCollectionStats.DeferredDestroyObjectCount);
	}
}
