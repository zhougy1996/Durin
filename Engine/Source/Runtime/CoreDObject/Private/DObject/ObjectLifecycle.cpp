#include "DObject/ObjectLifecycle.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Internal/GarbageCollection.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "CoreGlobals.h"
#include "Misc/Time.h"
#include "Threading/RunnableThread.h"
#include "GCReferenceSchema.h"

namespace Durin
{
    namespace
    {
        struct FPurgeObserver { std::function<void(DObject&)> Callback; bool Active = true; };
        auto PurgeObservers() -> std::map<uint64, std::shared_ptr<FPurgeObserver>>&
        { static std::map<uint64, std::shared_ptr<FPurgeObserver>> Values; return Values; }
        uint64 NextPurgeObserver = 1;
    }
    FObjectReferencePurgeRegistration::FObjectReferencePurgeRegistration(std::function<void(DObject&)> Callback)
        : Identity(NextPurgeObserver++)
    {
        CheckGameThread(); require(Callback);
        PurgeObservers().emplace(Identity, std::make_shared<FPurgeObserver>(FPurgeObserver{std::move(Callback)}));
    }
    FObjectReferencePurgeRegistration::~FObjectReferencePurgeRegistration()
    {
        CheckGameThread(); auto It = PurgeObservers().find(Identity);
        if (It != PurgeObservers().end()) { It->second->Active = false; PurgeObservers().erase(It); }
    }
    auto NotifyObjectReferencesInvalidated(DObject& Object) -> void
    {
        CheckGameThread(); const auto Snapshot = PurgeObservers();
        for (const auto& [Id, Observer] : Snapshot) if (Observer->Active) Observer->Callback(Object);
    }

	namespace
	{
		FGarbageCollectionStats GLastGarbageCollectionStats;
		bool GIsGarbageCollecting = false;
		std::optional<bool> GCollectionRequest;
		std::deque<DObject*> GPendingPurge;
		std::deque<DObject*> GPendingDelete;

		// Keeps the collector single-entry while object lifecycle callbacks run.
		class FGarbageCollectionScope
		{
		public:
			FGarbageCollectionScope()
			{
				requiref(!GIsGarbageCollecting, "Garbage collection cannot be entered recursively.");
				GIsGarbageCollecting = true;
			}

			~FGarbageCollectionScope()
			{
				GIsGarbageCollecting = false;
			}
		};

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

		auto MarkGarbageInternal(DObject* Object, bool bAllowTemplate = false) -> void
		{
			if (!Object || !GDObjectArray.Contains(Object) || IsPermanentObject(Object) || Object->IsGarbage()
				|| (Object->IsTemplateObject() && !bAllowTemplate)) return;
			Object->SetInternalFlags(EObjectInternalFlags::Garbage);
			if (auto* Package = Cast<DPackage>(Object->GetOuter())) Package->UnregisterTopLevelAsset(Object);
			if (Cast<DPackage>(Object)) Private::NotifyAssetPackageChanged();
            NotifyObjectReferencesInvalidated(*Object);
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
				for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::IncludeUnpublished, true))
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
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::IncludeUnpublished, true))
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

	auto AddToRoot(DObject* Object) -> void
	{
		CheckObjectThread();
		if (!GDObjectArray.Contains(Object) || Object->IsTemplateObject()) return;
		Object->SetInternalFlags(EObjectInternalFlags::RootSet);
	}

	auto RemoveFromRoot(DObject* Object) -> void
	{
		CheckObjectThread();
		if (!GDObjectArray.Contains(Object)) return;
		Object->ClearInternalFlags(EObjectInternalFlags::RootSet);
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
		if (!RootObject || !GDObjectArray.Contains(RootObject) || RootObject->IsTemplateObject()) return;

		// Marking changes flags/counters only, so the Outer index remains stable.
		// Follow its sibling back-pointers instead of allocating rollback storage or
		// recursing on a potentially deep partially constructed hierarchy.
		DObject* Current = RootObject;
		for (;;)
		{
			MarkGarbageInternal(Current);
			const auto Children = GDObjectArray.OuterToObjects.find(Current);
			if (Children != GDObjectArray.OuterToObjects.end() && !Children->second.empty())
			{
				Current = Children->second.front();
				continue;
			}
			while (Current != RootObject)
			{
				const auto Slot = GDObjectArray.ObjectToSlot.find(Current);
				const auto NextIndex = static_cast<size_t>(GDObjectArray.Slots[Slot->second].OuterIndex) + 1;
				Current = Current->GetOuter();
				const auto& Siblings = GDObjectArray.OuterToObjects.find(Current)->second;
				if (NextIndex < Siblings.size())
				{
					Current = Siblings[NextIndex];
					break;
				}
			}
			if (Current == RootObject) break;
		}
	}

	namespace Private
	{
		auto ReleaseDStructDefaultOwnership(DStruct* Struct) -> void
		{
			if (Struct) Struct->DestroyDefaultStorage(Struct->ReleaseDefaultValue());
		}

		auto ReleaseClassDefaultObjectOwnership(DClass* Class) -> DObject*
		{
			return Class ? Class->ReleaseDefaultObjectOwnership() : nullptr;
		}

		auto MarkTemplateObjectHierarchyAsGarbage(DObject* RootObject) -> void
		{
			CheckObjectThread();
			if (!RootObject || !GDObjectArray.Contains(RootObject) || !RootObject->IsTemplateObject()) return;

			std::vector<DObject*> Pending = {RootObject};
			while (!Pending.empty())
			{
				DObject* Object = Pending.back();
				Pending.pop_back();
				for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::IncludeUnpublished, true)) Pending.push_back(Child);
				MarkGarbageInternal(Object, true);
			}
		}
	}

	namespace
	{
		template<typename Predicate>
		auto ReleaseMatchingClassDefaultObjects(Predicate&& Matches) -> std::vector<DObject*>
		{
			std::vector<DClass*> Classes;
			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (auto* Class = Cast<DClass>(Object); Class && Matches(Class)) Classes.push_back(Class);
			}
			auto Depth = [](const DClass* Class) {
				uint32 Result = 0;
				for (; Class; Class = Class->GetSuperClass()) ++Result;
				return Result;
			};
			std::ranges::sort(Classes, [&](const DClass* Left, const DClass* Right) {
				const uint32 LeftDepth = Depth(Left);
				const uint32 RightDepth = Depth(Right);
				if (LeftDepth != RightDepth) return LeftDepth > RightDepth;
				return Left->GetQualifiedName().ToString() > Right->GetQualifiedName().ToString();
			});

			std::vector<DObject*> ReleasedObjects;
			for (DClass* Class : Classes)
			{
				if (DObject* Object = Private::ReleaseClassDefaultObjectOwnership(Class))
				{
					ReleasedObjects.push_back(Object);
					Private::MarkTemplateObjectHierarchyAsGarbage(Object);
				}
			}
			return ReleasedObjects;
		}
	}

	auto ReleaseClassDefaultObjects() -> void
	{
		CheckObjectThread();
		(void)ReleaseMatchingClassDefaultObjects([](const DClass*) { return true; });
	}

	void Private::ReleaseClassDefaultObjectForTests(DClass* Class)
	{
		CheckObjectThread();
		if (DObject* Object = Private::ReleaseClassDefaultObjectOwnership(Class))
		{
			Private::MarkTemplateObjectHierarchyAsGarbage(Object);
		}
	}

	auto ReleaseClassDefaultObjectsForModule(FName ModuleName) -> bool
	{
		CheckObjectThread();
		const std::string ModulePackagePath = std::format("/Cpp/{}", ModuleName);
		auto IsOwnedByModule = [&](const DObject* Object) {
			if (!Object || !Object->IsTemplateObject()) return false;
			const DObject* Outer = Object;
			while (Outer && !Cast<DClass>(Outer)) Outer = Outer->GetOuter();
			const auto* Class = Cast<DClass>(Outer);
			const DPackage* Package = Class ? Class->GetPackage() : nullptr;
			return Package && Package->GetPackagePath() == ModulePackagePath;
		};
		const std::vector<DObject*> ReleasedObjects = ReleaseMatchingClassDefaultObjects(
			[&](const DClass* Class) {
				const DPackage* Package = Class->GetPackage();
				return Package && Package->GetPackagePath() == ModulePackagePath;
			});
		bool bHasModuleTemplates = !ReleasedObjects.empty();
		if (!bHasModuleTemplates)
		{
			bHasModuleTemplates = std::ranges::any_of(
				GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished), IsOwnedByModule);
		}
		if (!bHasModuleTemplates) return true;

		if (IsIncrementalPurgePending()) IncrementalPurgeGarbage();
		if (!IsIncrementalPurgePending()) CollectGarbage(EObjectFlags::Standalone, false);
		return !std::ranges::any_of(
			GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished), IsOwnedByModule);
	}

	namespace
	{
		template<typename Predicate>
		auto ReleaseMatchingDStructDefaults(Predicate&& Matches) -> void
		{
			std::vector<DStruct*> Structs;
			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (auto* Struct = Cast<DStruct>(Object); Struct && Matches(Struct)) Structs.push_back(Struct);
			}
			std::ranges::sort(Structs, [](const DStruct* Left, const DStruct* Right) {
				return Left->GetQualifiedName().ToString() > Right->GetQualifiedName().ToString();
			});
			for (DStruct* Struct : Structs)
			{
				Private::ReleaseDStructDefaultOwnership(Struct);
			}
		}
	}

	auto ReleaseDStructDefaults() -> void
	{
		CheckObjectThread();
		ReleaseMatchingDStructDefaults([](const DStruct*) { return true; });
	}

	auto ReleaseDStructDefaultsForModule(FName ModuleName) -> void
	{
		CheckObjectThread();
		const std::string ModulePackagePath = std::format("/Cpp/{}", ModuleName);
		ReleaseMatchingDStructDefaults([&](const DStruct* Struct) {
			const DPackage* Package = Struct->GetPackage();
			return Package && Package->GetPackagePath() == ModulePackagePath;
		});
	}

	COREDOBJECT_API auto ConditionallyMarkAsReachable(DObject* Object) -> void
	{
		// Reachability is synchronous. This handle hook is not a write barrier.
		(void)Object;
	}

	auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void
	{
		if (!Object || !Object->GetClass()) return;
		Private::FGCReferenceSchemaRegistry::Visit(Object->GetClass(), Object, Collector);
	}

	auto RequestGarbageCollection(bool bForcePurge) -> void
	{
		CheckObjectThread();
		GCollectionRequest = GCollectionRequest.value_or(false) || bForcePurge;
	}

	auto IsGarbageCollecting() -> bool { CheckObjectThread(); return GIsGarbageCollecting; }
	auto IsGarbageCollectionRequested() -> bool { CheckObjectThread(); return GCollectionRequest.has_value(); }
	auto Private::IsFullPurgeRequested() -> bool { CheckObjectThread(); return GCollectionRequest.value_or(false); }
	auto IsIncrementalPurgePending() -> bool { CheckObjectThread(); return !GPendingPurge.empty() || !GPendingDelete.empty(); }

	namespace
	{
		// Finish a ready batch before deleting it. Waiting objects rotate fairly.
		auto PurgeGarbage(bool bUseTimeLimit, double TimeLimit) -> void
		{
			const double Start = FTime::Seconds();
			bool bDidWork = false;
			auto BudgetExpired = [&] {
				return bUseTimeLimit && bDidWork && FTime::Seconds() - Start >= TimeLimit;
			};
			auto DeleteReady = [&] {
				while (!GPendingDelete.empty() && !BudgetExpired())
				{
					DObject* Object = GPendingDelete.front();
					GPendingDelete.pop_front();
					DestroyObject(Object);
					++GLastGarbageCollectionStats.SweptObjectCount;
					bDidWork = true;
				}
			};
			// Complete the prior ready batch first, even with a zero budget.
			DeleteReady();
			do
			{
				const size_t PassSize = GPendingPurge.size();
				for (size_t Visited = 0; Visited < PassSize && !BudgetExpired(); ++Visited)
				{
					DObject* Object = GPendingPurge.front();
					GPendingPurge.pop_front();
					if (Object->IsReadyForFinishDestroy())
					{
						Object->FinishDestroy();
						Object->SetInternalFlags(EObjectInternalFlags::FinishDestroyed);
						GPendingDelete.push_back(Object);
					}
					else GPendingPurge.push_back(Object);
					bDidWork = true;
				}
				DeleteReady();
				if (bUseTimeLimit) break;
				if (!GPendingPurge.empty()) std::this_thread::yield();
			} while (!GPendingPurge.empty());
			GLastGarbageCollectionStats.DeferredDestroyObjectCount = GPendingPurge.size() + GPendingDelete.size();
			GLastGarbageCollectionStats.SweepMilliseconds += (FTime::Seconds() - Start) * 1000.0;
		}
	}

	auto IncrementalPurgeGarbage(bool bUseTimeLimit, double TimeLimit) -> void
	{
		CheckObjectThread();
		const FGarbageCollectionScope CollectionScope;
		PurgeGarbage(bUseTimeLimit, TimeLimit);
	}

	auto TryCollectGarbage(EObjectFlags KeepFlags, bool bPerformFullPurge) -> bool
	{
		if (IsGarbageCollecting()) return false;
		CollectGarbage(KeepFlags, bPerformFullPurge);
		return true;
	}

	auto CollectGarbage(EObjectFlags KeepFlags, bool bPerformFullPurge) -> void
	{
		Private::CollectGarbageWithPostReachability(KeepFlags, bPerformFullPurge, {});
	}

	auto Private::CollectGarbageWithPostReachability(EObjectFlags KeepFlags, bool bPerformFullPurge,
		const std::function<void()>& PostReachability) -> void
	{
		CheckObjectThread();
		requiref(!IsGarbageCollecting(),
			"CollectGarbage cannot run from collector callbacks; request GC instead.");
		// Complete the previous cycle before starting another reachability snapshot.
		const FGarbageCollectionScope CollectionScope;
		if (!GCollectionRequest.value_or(false) || bPerformFullPurge) GCollectionRequest.reset();
		PurgeGarbage(false, 0.0);
		const uint64 Serial = GLastGarbageCollectionStats.CollectionSerial + 1;
		GLastGarbageCollectionStats = {};
		GLastGarbageCollectionStats.CollectionSerial = Serial;
		GLastGarbageCollectionStats.CollectionFrame = GFrameCounter;
		const uint64 ObjectCountBeforeCollection = GDObjectArray.GetNum();
		const uint64 PendingKillCountBeforeCollection = GetGarbageObjectCount();
		const double CollectionStartTime = FTime::Seconds();
		const double MarkStartTime = FTime::Seconds();
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished)) Object->ClearInternalFlags(EObjectInternalFlags::Reachable);

		FMarkReferenceCollector Marker;
		Private::AddStrongObjectReferences(Marker);
		Private::AddFunctionParameterReferences(Marker);
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
		{
			if (!Object->IsGarbage()
				&& (Object->HasAnyInternalFlags(EObjectInternalFlags::RootSet)
					|| Object->HasAnyObjectFlags(KeepFlags)
					|| IsPermanentObject(Object)))
			{
				Marker.Enqueue(Object);
			}
		}
		GLastGarbageCollectionStats.MarkedObjectCount = Marker.Drain();
		if (PostReachability) PostReachability();
		GLastGarbageCollectionStats.MarkMilliseconds = (FTime::Seconds() - MarkStartTime) * 1000.0;

		std::vector<DObject*> SweepCandidates;
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
		{
			if (!IsPermanentObject(Object) && (Object->IsGarbage() || !Object->HasAnyInternalFlags(EObjectInternalFlags::Reachable)))
			{
				SweepCandidates.push_back(Object);
			}
		}
		GLastGarbageCollectionStats.CandidateObjectCount = static_cast<uint64>(SweepCandidates.size());

		const double SweepStartTime = FTime::Seconds();
		std::vector<DObject*> DestroyOrder = GatherDestroyOrder(SweepCandidates);
		for (DObject* Object : DestroyOrder)
		{
			// An unreachable export may leave its package and sibling exports alive.
			if (!Object->IsGarbage())
			{
				DPackage* Package = Object->GetPackage();
				if (Package && Package != Object && Package->IsAssetPackage()
					&& Package->HasAnyInternalFlags(EObjectInternalFlags::Reachable)
					&& (Object->GetConstructionPurpose() == EObjectConstructionPurpose::AssetLoad
						|| (Object->GetOuter() == Package && Package->FindTopLevelAsset(Object->GetFName()) == Object)))
					Package->SetFullyLoaded(false);
			}
			MarkGarbageInternal(Object);
		}
		for (auto It = DestroyOrder.rbegin(); It != DestroyOrder.rend(); ++It)
		{
			DObject* Object = *It;
			if (Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed)) continue;
			Object->SetInternalFlags(EObjectInternalFlags::BeginDestroyed);
			Object->BeginDestroy();
		}

		GPendingPurge.assign(DestroyOrder.begin(), DestroyOrder.end());
		GLastGarbageCollectionStats.SweepMilliseconds = (FTime::Seconds() - SweepStartTime) * 1000.0;
		PurgeGarbage(!bPerformFullPurge, 0.002);
		GLastGarbageCollectionStats.CollectionTime = FTime::Seconds();
		GLastGarbageCollectionStats.ObjectCountAfterCollection = GDObjectArray.GetNum();
		DURIN_INFO_CATEGORY(
			"GC",
			"Collected: {:.2f} ms (mark {:.2f}, purge {:.2f}) | objects {} -> {} | "
			"reachable={} candidates={} freed={} pending={} garbage={}",
			(FTime::Seconds() - CollectionStartTime) * 1000.0,
			GLastGarbageCollectionStats.MarkMilliseconds,
			GLastGarbageCollectionStats.SweepMilliseconds,
			ObjectCountBeforeCollection,
			GDObjectArray.GetNum(),
			GLastGarbageCollectionStats.MarkedObjectCount,
			GLastGarbageCollectionStats.CandidateObjectCount,
			GLastGarbageCollectionStats.SweptObjectCount,
			GLastGarbageCollectionStats.DeferredDestroyObjectCount,
			PendingKillCountBeforeCollection);
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
		if (!IsIncrementalPurgePending())
		{
			return;
		}

		for (const DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
		{
			if (!Object
				|| !Object->HasAnyInternalFlags(
					EObjectInternalFlags::BeginDestroyed))
			{
				continue;
			}
			DURIN_ERROR_CATEGORY(
				"GC",
				"Object destruction remained pending during '{}': path='{}', class='{}', "
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
