#include "DObject/ObjectGraphReplacement.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		using K = DurinCodeGen::EPropertyGenFlags;
		using E = EObjectReplacementError;
		bool GReplacementActive = false;
		bool GReplacementExecuting = false;

		// Callback reentry may observe Busy but cannot consume a partially validated plan.
		struct FExecutionScope
		{
			FExecutionScope() { GReplacementExecuting = true; }
			~FExecutionScope() { GReplacementExecuting = false; }
		};

		auto CheckThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto Fail(E Error, std::string Message) -> FObjectReplacementResult
		{
			return {Error, std::move(Message)};
		}

		auto RelativeNames(const DObject* Object, const DPackage* Package) -> std::vector<FName>
		{
			std::vector<FName> Names;
			for (; Object && Object != Package; Object = Object->GetOuter()) Names.push_back(Object->GetFName());
			return Names;
		}

		struct FRelativeNamesHash
		{
			auto operator()(const std::vector<FName>& Names) const -> size_t
			{
				size_t Hash = 0;
				for (FName Name : Names) Hash ^= std::hash<FName>{}(Name) + 0x9e3779b9 + (Hash << 6) + (Hash >> 2);
				return Hash;
			}
		};

		auto HasReferenceMetadata(FProperty* P, uint32 Depth = 0) -> bool
		{
			if (!P || Depth > 64) return true;
			switch (P->GetKind())
			{
			case K::Object: return true;
			case K::Array: return HasReferenceMetadata(static_cast<FArrayProperty*>(P)->GetInner(), Depth + 1);
			case K::Map:
				return HasReferenceMetadata(static_cast<FMapProperty*>(P)->GetKeyProp(), Depth + 1)
					|| HasReferenceMetadata(static_cast<FMapProperty*>(P)->GetValueProp(), Depth + 1);
			case K::Struct:
			{
				auto* Type = static_cast<FStructProperty*>(P)->GetStruct();
				if (!Type || Type->HasReferenceCollector()) return true;
				bool HasReferences = false;
				Type->ForEachProperty([&](FProperty* Field) { HasReferences |= HasReferenceMetadata(Field, Depth + 1); });
				return HasReferences;
			}
			default: return false;
			}
		}

		// Includes storage identity so container reallocations cannot leave a stale write address.
		struct FReferenceEdge
		{
			DObject* Owner;
			const FProperty* Property;
			const void* Address;
			DObject* Value;
			auto operator==(const FReferenceEdge&) const -> bool = default;
		};

		struct FSlotWrite
		{
			FObjectProperty* Property;
			void* Container;
			uint32 Index;
			DObject* Replacement;
		};

		struct FContainerWrite
		{
			FProperty* Property;
			void* Container;
			uint32 Index;
			FReflectedValueStorage Before;
			FReflectedValueStorage After;
		};

		// Map equality uses its own key lookup, including native object-reference keys.
		// Canonical disk Map-key restrictions do not govern in-memory reference repair.
		auto Equal(FProperty* P, const void* A, uint32 AI, const void* B, uint32 BI) -> bool;
		auto EqualStruct(DStruct* Type, const void* A, const void* B) -> bool
		{
			bool Result = true;
			Type->ForEachProperty([&](FProperty* P) {
				for (uint32 I = 0; Result && I < P->GetArrayDim(); ++I) Result = Equal(P, A, I, B, I);
			});
			return Result;
		}

		auto Equal(FProperty* P, const void* A, uint32 AI, const void* B, uint32 BI) -> bool
		{
			if (P->GetKind() == K::Array)
			{
				auto* Array = static_cast<FArrayProperty*>(P);
				const auto& Ops = Array->GetOps();
				const void* AV = P->GetValuePtr(A, AI);
				const void* BV = P->GetValuePtr(B, BI);
				if (!Ops.Num || !Ops.GetConstAt || Ops.Num(AV) != Ops.Num(BV)) return false;
				for (uint64 I = 0; I < Ops.Num(AV); ++I)
				{
					const void* Left = nullptr; const void* Right = nullptr;
					if (Ops.GetConstAt(AV, I, &Left) != EContainerOpResult::Success
						|| Ops.GetConstAt(BV, I, &Right) != EContainerOpResult::Success
						|| !Equal(Array->GetInner(), Left, 0, Right, 0)) return false;
				}
				return true;
			}
			if (P->GetKind() == K::Map)
			{
				auto* Map = static_cast<FMapProperty*>(P);
				const auto& Ops = Map->GetOps();
				const void* AV = P->GetValuePtr(A, AI);
				const void* BV = P->GetValuePtr(B, BI);
				if (!Ops.Num || !Ops.VisitConst || !Ops.Lookup || Ops.Num(AV) != Ops.Num(BV)) return false;
				struct FContext { FMapProperty* Map; const void* Other; bool Same = true; } Context{Map, BV};
				const auto Result = Ops.VisitConst(AV, [](void* Raw, const void* Key, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					const void* Other = nullptr;
					C.Same = C.Map->GetOps().Lookup(C.Other, Key, &Other) == EContainerOpResult::Success
						&& Equal(C.Map->GetValueProp(), Value, 0, Other, 0);
					return C.Same;
				}, &Context);
				return Result == EContainerOpResult::Success && Context.Same;
			}
			if (P->GetKind() == K::Struct)
			{
				auto* Type = static_cast<FStructProperty*>(P)->GetStruct();
				if (!Type->HasIdentical()) return EqualStruct(Type, P->GetValuePtr(A, AI), P->GetValuePtr(B, BI));
			}
			return ComparePropertyValues(P, A, AI, B, BI) == EPropertyIdentityResult::Identical;
		}

		// Mutates detached storage only. Map keys are copied before modification and
		// inserted into a separate index; no const key is ever modified in place.
		auto Rewrite(FProperty* P, void* Container, uint32 Index,
			const FObjectReplacementMap& Map) -> FObjectReplacementResult
		{
			if (!HasReferenceMetadata(P)) return {};
			if (P->GetKind() == K::Object)
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(P);
				if (const auto* Entry = Map.Find(ObjectProperty->GetObjectPropertyValue(Container, Index)))
				{
					if (!Entry->Replacement) return Fail(E::UnmappedReference, "Detached value has an unmapped reference.");
					ObjectProperty->SetObjectPropertyValue(Container, Entry->Replacement, Index);
				}
			}
			else if (P->GetKind() == K::Struct)
			{
				auto* Struct = static_cast<FStructProperty*>(P);
				FObjectReplacementResult Result;
				Struct->GetStruct()->ForEachProperty([&](FProperty* Field) {
					for (uint32 I = 0; Result && I < Field->GetArrayDim(); ++I)
						Result = Rewrite(Field, P->GetValuePtr(Container, Index), I, Map);
				});
				return Result;
			}
			else if (P->GetKind() == K::Array)
			{
				auto* Array = static_cast<FArrayProperty*>(P);
				if (!Array->HasArrayOps() || !Array->GetOps().VisitMutable)
					return Fail(E::Unsupported, "Array lacks mutable detached traversal.");
				struct FContext { FProperty* Inner; const FObjectReplacementMap& Map; FObjectReplacementResult Result; }
					Context{Array->GetInner(), Map, {}};
				const auto Result = Array->VisitMutableElements(Container, [](void* Raw, uint64, void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					C.Result = Rewrite(C.Inner, Value, 0, C.Map);
					return bool(C.Result);
				}, &Context, Index);
				if (!Context.Result) return Context.Result;
				if (Result != EContainerOpResult::Success) return Fail(E::Unsupported, "Array traversal failed.");
			}
			else if (P->GetKind() == K::Map)
			{
				auto* Property = static_cast<FMapProperty*>(P);
				if (!Property->HasMapOps()) return Fail(E::Unsupported, "Map has no operations.");
				const auto& Ops = Property->GetOps();
				if (!Ops.CreateDetached || !Ops.DestroyDetached || !Ops.Commit || !Ops.InsertCopy || !Ops.VisitConst)
					return Fail(E::Unsupported, "Map lacks transactional detached operations.");
				void* Detached = nullptr;
				if (Ops.CreateDetached(&Detached) != EContainerOpResult::Success)
					return Fail(E::AllocationFailure, "Could not allocate detached Map.");
				std::unique_ptr<void, decltype(Ops.DestroyDetached)> Storage(Detached, Ops.DestroyDetached);
				struct FContext { FMapProperty* Property; const FObjectReplacementMap& Map; void* Storage; FObjectReplacementResult Result; }
					Context{Property, Map, Detached, {}};
				const auto Result = Property->VisitEntries(Container, [](void* Raw, const void* Key, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					FReflectedValueStorage KCopy, VCopy;
					if (!KCopy.CopyConstruct(C.Property->GetKeyProp(), C.Property->GetKeyProp()->GetValuePtr(Key))
						|| !VCopy.CopyConstruct(C.Property->GetValueProp(), C.Property->GetValueProp()->GetValuePtr(Value)))
					{
						C.Result = Fail(E::Unsupported, "Map key/value cannot be copied."); return false;
					}
					C.Result = Rewrite(C.Property->GetKeyProp(), KCopy.GetContainer(), 0, C.Map);
					if (C.Result) C.Result = Rewrite(C.Property->GetValueProp(), VCopy.GetContainer(), 0, C.Map);
					if (!C.Result) return false;
					const auto Insert = C.Property->GetOps().InsertCopy(C.Storage, KCopy.GetValue(), VCopy.GetValue());
					if (Insert != EContainerOpResult::Success)
						C.Result = Fail(Insert == EContainerOpResult::DuplicateKey ? E::MapCollision : E::Unsupported,
							"Replacement Map key collides or insertion failed.");
					return bool(C.Result);
				}, &Context, Index);
				if (!Context.Result) return Context.Result;
				if (Result != EContainerOpResult::Success) return Fail(E::Unsupported, "Map traversal failed.");
				const auto Commit = Ops.Commit(P->GetValuePtr(Container, Index), Detached);
				require(Commit == EContainerOpResult::Success);
			}
			return {};
		}
	}

	auto FObjectReplacementMap::Find(DObject* Previous) const -> const FEntry*
	{
		const auto It = Indices.find(Previous);
		return It == Indices.end() ? nullptr : &Entries[It->second];
	}

	auto FObjectReplacementMap::Build(std::span<const FObjectReplacementPackagePair> Packages,
		const FObjectReplacementBudget& Budget) -> FObjectReplacementResult
	{
		CheckThread();
		FObjectReplacementMap Candidate;
		if (Packages.empty() || Packages.size() > Budget.MaximumPackages)
			return Fail(E::BudgetExceeded, "Replacement package count is empty or exceeds its budget.");
		const auto All = GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished);
		std::unordered_set<DPackage*> Seen;
		for (const auto& Pair : Packages)
		{
			if (!IsValid(Pair.Current) || !IsValid(Pair.Prepared) || Pair.Current == Pair.Prepared
				|| !Pair.Current->IsAssetPackage() || Pair.Current->IsGraphPrivate()
				|| !Pair.Prepared->IsPreparedAssetPackage()
				|| FindPackage(Pair.Prepared->GetPackagePath()) != Pair.Current
				|| !Seen.insert(Pair.Current).second || !Seen.insert(Pair.Prepared).second)
				return Fail(E::InvalidGraph, "Expected unique live/prepared package pairs at the same path.");
			std::unordered_map<std::vector<FName>, DObject*, FRelativeNamesHash> Old, New;
			for (DObject* Object : All)
			{
				if (Object->GetPackage() != Pair.Current && Object->GetPackage() != Pair.Prepared) continue;
				if (!IsValid(Object) || Object->IsTemplateObject()) return Fail(E::InvalidGraph, "Graph contains a dead/template object.");
				auto& Graph = Object->GetPackage() == Pair.Current ? Old : New;
				auto Names = RelativeNames(Object, Object->GetPackage());
				if (!Graph.emplace(std::move(Names), Object).second)
					return Fail(E::InvalidGraph, "Graph contains duplicate relative Outer paths.");
				if (Old.size() + New.size() + Candidate.Entries.size() + Candidate.PreparedObjects.size() > Budget.MaximumObjects)
					return Fail(E::BudgetExceeded, "Replacement graph object budget exceeded.");
			}
			for (const auto& [Names, Object] : Old)
			{
				const auto It = New.find(Names);
				DObject* Replacement = It == New.end() ? nullptr : It->second;
				if (Replacement && !Replacement->IsA(Object->GetClass()))
					return Fail(E::IncompatibleType, "Replacement type is not assignable to the previous type.");
				Candidate.Indices.emplace(Object, Candidate.Entries.size());
				Candidate.Entries.push_back({Object, Replacement});
			}
			for (const auto& Item : New) Candidate.PreparedObjects.push_back(Item.second);
		}
		*this = std::move(Candidate);
		return {};
	}

	struct FObjectReferenceReplacementPlan::FImpl
	{
		std::vector<FReferenceEdge> Edges;
		std::vector<FSlotWrite> Slots;
		std::vector<FContainerWrite> Containers;
		uint64 MaximumSlots = 0;

		auto ScanProperty(DObject* Owner, FProperty* P, void* Container, uint32 Index,
			const FObjectReplacementMap& Map, bool bDetachedAncestor, bool& Changed, uint32 Depth = 0) -> FObjectReplacementResult
		{
			if (!P || Depth > 64) return Fail(E::Unsupported, "Reference metadata is missing or exceeds the traversal depth limit.");
			if (!HasReferenceMetadata(P)) return {};
			if (P->GetKind() == K::Object)
			{
				auto* Property = static_cast<FObjectProperty*>(P);
				DObject* Value = Property->GetObjectPropertyValue(Container, Index);
				if (Edges.size() >= MaximumSlots) return Fail(E::BudgetExceeded, "Reference slot budget exceeded.");
				Edges.push_back({Owner, P, P->GetValuePtr(Container, Index), Value});
				if (const auto* Entry = Map.Find(Value))
				{
					if (!Property->HasObjectValueWriter()) return Fail(E::Unsupported, "Reference property has no writer.");
					if (Owner->IsTemplateObject()) return Fail(E::Unsupported, "Templates cannot reference a replaced live graph.");
					if (!Entry->Replacement) return Fail(E::UnmappedReference, "A graph-external reference has no replacement.");
					if (P->GetReferencedClass() && !Entry->Replacement->IsA(P->GetReferencedClass()))
						return Fail(E::IncompatibleType, "Replacement is incompatible with a reference property.");
					Changed = true;
					if (!bDetachedAncestor) Slots.push_back({Property, Container, Index, Entry->Replacement});
				}
				return {};
			}
			if (P->GetKind() == K::Struct)
			{
				auto* Type = static_cast<FStructProperty*>(P)->GetStruct();
				if (!Type) return Fail(E::Unsupported, "Struct metadata is missing.");
				FObjectReplacementResult Result;
				bool StructChanged = false;
				Type->ForEachProperty([&](FProperty* Field) {
					for (uint32 I = 0; Result && I < Field->GetArrayDim(); ++I)
						Result = ScanProperty(Owner, Field, P->GetValuePtr(Container, Index), I, Map, bDetachedAncestor, StructChanged, Depth + 1);
				});
				Changed |= StructChanged;
				if (StructChanged && Type->HasReferenceCollector())
					return Fail(E::Unsupported, "A reference-bearing native struct needs a dedicated replacement adapter.");
				return Result;
			}
			if (P->GetKind() != K::Array && P->GetKind() != K::Map) return {};
			bool LocalChanged = false;
			struct FContext
			{
				FImpl& Plan; DObject* Owner; FProperty* P; const FObjectReplacementMap& Map;
				bool& Changed; FObjectReplacementResult Result; uint32 Depth;
			} Context{*this, Owner, P, Map, LocalChanged, {}, Depth};
			EContainerOpResult Traversal;
			if (P->GetKind() == K::Array)
			{
				auto* Array = static_cast<FArrayProperty*>(P);
				if (!Array->HasArrayOps() || !Array->GetInner() || !Array->GetOps().VisitConst)
					return Fail(E::Unsupported, "Array lacks reference traversal metadata.");
				Traversal = Array->VisitElements(Container, [](void* Raw, uint64, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					C.Result = C.Plan.ScanProperty(C.Owner, static_cast<FArrayProperty*>(C.P)->GetInner(),
						const_cast<void*>(Value), 0, C.Map, true, C.Changed, C.Depth + 1);
					return bool(C.Result);
				}, &Context, Index);
			}
			else
			{
				auto* Property = static_cast<FMapProperty*>(P);
				if (!Property->HasMapOps() || !Property->GetKeyProp() || !Property->GetValueProp() || !Property->GetOps().VisitConst)
					return Fail(E::Unsupported, "Map lacks reference traversal metadata.");
				Traversal = Property->VisitEntries(Container, [](void* Raw, const void* Key, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					auto* M = static_cast<FMapProperty*>(C.P);
					C.Result = C.Plan.ScanProperty(C.Owner, M->GetKeyProp(), const_cast<void*>(Key), 0, C.Map, true, C.Changed, C.Depth + 1);
					if (C.Result) C.Result = C.Plan.ScanProperty(C.Owner, M->GetValueProp(), const_cast<void*>(Value), 0, C.Map, true, C.Changed, C.Depth + 1);
					return bool(C.Result);
				}, &Context, Index);
			}
			if (!Context.Result) return Context.Result;
			if (Traversal != EContainerOpResult::Success) return Fail(E::Unsupported, "Reference traversal failed.");
			Changed |= LocalChanged;
			if (!LocalChanged || bDetachedAncestor) return {};
			const bool bCanCommit = P->GetKind() == K::Array
				? static_cast<FArrayProperty*>(P)->HasCapability(EArrayOpsFlags::TransactionalCommit)
				: static_cast<FMapProperty*>(P)->HasCapability(EMapOpsFlags::TransactionalCommit);
			if (!bCanCommit) return Fail(E::Unsupported, "Container has no nonthrowing commit adapter.");
			FContainerWrite Write{P, Container, Index, {}, {}};
			if (!Write.Before.CopyConstruct(P, P->GetValuePtr(Container, Index), Index)
				|| !Write.After.CopyConstruct(P, P->GetValuePtr(Container, Index), Index))
				return Fail(E::Unsupported, "Reference container cannot be copied for atomic publication.");
			if (!Equal(P, Container, Index, Write.Before.GetContainer(), Index))
				return Fail(E::Unsupported, "Reference container cannot be compared for stale validation.");
			auto Result = Rewrite(P, Write.After.GetContainer(), Index, Map);
			if (!Result) return Result;
			Containers.push_back(std::move(Write));
			return {};
		}

		auto Scan(const FObjectReplacementMap& Map,
			std::span<const std::shared_ptr<IObjectReplacementParticipant>> Participants) -> FObjectReplacementResult
		{
			for (DObject* Owner : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (!IsValid(Owner) || Map.Find(Owner)) continue;
				const size_t Begin = Edges.size();
				FObjectReplacementResult Result;
				bool Changed = false;
				Owner->GetClass()->ForEachProperty([&](FProperty* P) {
					for (uint32 I = 0; Result && I < P->GetArrayDim(); ++I)
						Result = ScanProperty(Owner, P, Owner, I, Map, false, Changed);
				});
				if (!Result) return Result;
				std::unordered_map<DObject*, uint64> Reflected;
				for (size_t I = Begin; I < Edges.size(); ++I)
					if (Edges[I].Property->IsObjectPtrWrapper() && Map.Find(Edges[I].Value)) ++Reflected[Edges[I].Value];
				class FNativeAudit final : public FReferenceCollector
				{
				public:
					const FObjectReplacementMap& Map;
					std::unordered_map<DObject*, uint64> Counts;
					explicit FNativeAudit(const FObjectReplacementMap& InMap) : Map(InMap) {}
					auto AddReferencedObject(DObject*& Value) -> void override { if (Map.Find(Value)) ++Counts[Value]; }
				} Audit(Map);
				Owner->AddReferencedObjects(Audit);
				const bool Native = std::ranges::any_of(Audit.Counts, [&](const auto& Pair) {
					return Pair.second > Reflected[Pair.first];
				});
				if (Native && !std::ranges::any_of(Participants, [&](const auto& P) { return P->CoversNativeReferences(*Owner); }))
					return Fail(E::Unsupported, "An enumerated native owner has no replacement participant.");
			}
			return {};
		}

		auto Commit() noexcept -> void
		{
			for (const auto& Write : Slots) Write.Property->SetObjectPropertyValue(Write.Container, Write.Replacement, Write.Index);
			for (auto& Write : Containers)
			{
				void* Live = Write.Property->GetValuePtr(Write.Container, Write.Index);
				const auto Result = Write.Property->GetKind() == K::Array
					? static_cast<FArrayProperty*>(Write.Property)->GetOps().Commit(Live, Write.After.GetValue())
					: static_cast<FMapProperty*>(Write.Property)->GetOps().Commit(Live, Write.After.GetValue());
				require(Result == EContainerOpResult::Success);
			}
		}
	};

	FObjectReferenceReplacementPlan::FObjectReferenceReplacementPlan() : Impl(std::make_unique<FImpl>()) {}
	FObjectReferenceReplacementPlan::~FObjectReferenceReplacementPlan() = default;

	struct FObjectGraphReplacement::FImpl
	{
		enum class EState { Empty, Prepared, Committed, Finished } State = EState::Empty;
		FObjectReplacementMap Map;
		FObjectReplacementBudget Budget;
		FObjectReferenceReplacementPlan References;
		std::vector<FObjectReplacementPackagePair> Packages;
		std::vector<std::shared_ptr<IObjectReplacementParticipant>> Participants;
		std::vector<FStrongObjectPtr> Pins;
		std::vector<std::pair<FObjectHandle, std::string>> Identities;
		std::vector<uint64> PackageRevisions;
		uint64 ArrayRevision = 0;

		auto CheckStrongOwners() const -> bool
		{
			for (const auto& Entry : Map.GetEntries())
			{
				uint64 Claimed = 1; // This operation pins every live object once.
				for (const auto& P : Participants) Claimed += P->GetStrongReferenceCount(*Entry.Previous);
				if (Private::GetStrongObjectReferenceCount(MakeObjectHandle(Entry.Previous)) != Claimed) return false;
			}
			return true;
		}
	};

	FObjectGraphReplacement::FObjectGraphReplacement() : Impl(std::make_unique<FImpl>()) {}
	FObjectGraphReplacement::~FObjectGraphReplacement()
	{
		require(Impl->State != FImpl::EState::Committed);
		Abort();
	}

	auto FObjectGraphReplacement::GetMap() const -> const FObjectReplacementMap& { return Impl->Map; }

	auto FObjectGraphReplacement::Prepare(std::span<const FObjectReplacementPackagePair> Packages,
		std::span<const std::shared_ptr<IObjectReplacementParticipant>> Participants,
		const FObjectReplacementBudget& Budget) -> FObjectReplacementResult
	{
		CheckThread();
		if (GReplacementActive || Impl->State != FImpl::EState::Empty) return Fail(E::Busy, "A replacement is already active.");
		FExecutionScope Execution;
		GReplacementActive = true;
		size_t PreparedParticipants = 0;
		auto Reject = [&](FObjectReplacementResult Result) {
			while (PreparedParticipants) Impl->Participants[--PreparedParticipants]->Abort();
			Impl->Pins.clear();
			Impl->Participants.clear();
			Impl->State = FImpl::EState::Finished;
			GReplacementActive = false;
			return Result;
		};
		try
		{
			auto Result = Impl->Map.Build(Packages, Budget);
			if (!Result) return Reject(std::move(Result));
			Impl->Budget = Budget;
			Impl->Packages.assign(Packages.begin(), Packages.end());
			Impl->Participants.assign(Participants.begin(), Participants.end());
			std::unordered_set<const IObjectReplacementParticipant*> Unique;
			for (const auto& P : Impl->Participants)
				if (!P || !Unique.insert(P.get()).second) return Reject(Fail(E::InvalidGraph, "Duplicate/null participant."));
			for (const auto& Entry : Impl->Map.GetEntries())
				if (Entry.Previous->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
					return Reject(Fail(E::Unsupported, "An explicitly rooted old object cannot be retired."));
			const auto PreparedObjects = Impl->Map.GetPreparedObjects();
			const std::unordered_set<DObject*> PreparedSet(PreparedObjects.begin(), PreparedObjects.end());
			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (!IsValid(Object)) continue;
				Impl->Pins.emplace_back(Object);
				if (Impl->Map.Find(Object) || PreparedSet.contains(Object))
					Impl->Identities.emplace_back(MakeObjectHandle(Object), Object->GetObjectPath());
			}
			for (const auto& Pair : Packages)
			{
				Impl->PackageRevisions.push_back(Pair.Current->GetEditRevision());
				Impl->PackageRevisions.push_back(Pair.Prepared->GetEditRevision());
			}
			for (const auto& P : Impl->Participants)
			{
				++PreparedParticipants;
				Result = P->Prepare(Impl->Map);
				if (!Result) return Reject(std::move(Result));
			}
			if (!Impl->CheckStrongOwners()) return Reject(Fail(E::Unsupported, "An external strong owner has no exact replacement claim."));
			Impl->References.Impl->MaximumSlots = Budget.MaximumReferenceSlots;
			Result = Impl->References.Impl->Scan(Impl->Map, Impl->Participants);
			if (!Result) return Reject(std::move(Result));
			Impl->ArrayRevision = GDObjectArray.GetRevision();
			Impl->State = FImpl::EState::Prepared;
			return {};
		}
		catch (const std::bad_alloc&) { return Reject(Fail(E::AllocationFailure, "Replacement preparation allocation failed.")); }
		catch (...) { return Reject(Fail(E::ParticipantRejected, "Replacement preparation callback threw.")); }
	}

	auto FObjectGraphReplacement::TryCommit() -> FObjectReplacementResult
	{
		CheckThread();
		if (GReplacementExecuting || Impl->State != FImpl::EState::Prepared) return Fail(E::Busy, "Replacement is not prepared or is executing.");
		FExecutionScope Execution;
		try
		{
			if (GDObjectArray.GetRevision() != Impl->ArrayRevision) return Fail(E::Stale, "Object membership changed during preparation.");
			for (const auto& [Handle, Path] : Impl->Identities)
			{
				DObject* Object = ResolveObjectHandle(Handle);
				if (!IsValid(Object) || Object->GetObjectPath() != Path) return Fail(E::Stale, "Graph identity changed.");
			}
			for (const auto& Entry : Impl->Map.GetEntries())
				if (Entry.Previous->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
					return Fail(E::Stale, "An old object acquired a manual root during preparation.");
			for (size_t I = 0; I < Impl->Packages.size(); ++I)
			{
				const auto& Pair = Impl->Packages[I];
				if (FindPackage(Pair.Prepared->GetPackagePath()) != Pair.Current
					|| !Pair.Prepared->IsPreparedAssetPackage()
					|| Pair.Current->GetEditRevision() != Impl->PackageRevisions[I * 2]
					|| Pair.Prepared->GetEditRevision() != Impl->PackageRevisions[I * 2 + 1])
					return Fail(E::Stale, "Package registration or edit revision changed.");
			}
			for (const auto& P : Impl->Participants) if (!P->Validate()) return Fail(E::Stale, "Native participant changed.");
			if (!Impl->CheckStrongOwners()) return Fail(E::Stale, "Strong ownership changed.");
			FObjectReferenceReplacementPlan Fresh;
			Fresh.Impl->MaximumSlots = Impl->Budget.MaximumReferenceSlots;
			auto Result = Fresh.Impl->Scan(Impl->Map, Impl->Participants);
			if (!Result) return Result;
			if (Fresh.Impl->Edges != Impl->References.Impl->Edges) return Fail(E::Stale, "Reference owners or slots changed.");
			for (const auto& Write : Impl->References.Impl->Containers)
				if (!Equal(Write.Property, Write.Container, Write.Index, Write.Before.GetContainer(), Write.Index))
					return Fail(E::Stale, "Reference container contents changed.");
			if (GDObjectArray.GetRevision() != Impl->ArrayRevision || !Impl->CheckStrongOwners())
				return Fail(E::Stale, "Validation changed object membership or ownership.");
			// All fallible traversal, copies, collision checks and participant validation precede this call.
			CommitPrepared();
			return {};
		}
		catch (const std::bad_alloc&) { return Fail(E::AllocationFailure, "Replacement validation allocation failed."); }
		catch (...) { return Fail(E::ParticipantRejected, "Replacement validation callback threw."); }
	}

	auto FObjectGraphReplacement::CommitPrepared() noexcept -> void
	{
		for (const auto& Pair : Impl->Packages)
		{
			Pair.Prepared->SetStandaloneResidency(Pair.Current->HasAnyObjectFlags(EObjectFlags::Standalone));
			Pair.Current->SetStandaloneResidency(false);
			Pair.Prepared->CommitPreparedPackageRegistration(*Pair.Current);
		}
		Impl->References.Impl->Commit();
		for (const auto& P : Impl->Participants) P->Commit();
		InvalidateSoftObjectCaches();
		Impl->State = FImpl::EState::Committed;
	}

	auto FObjectGraphReplacement::Abort() noexcept -> void
	{
		CheckThread();
		if (GReplacementExecuting || Impl->State != FImpl::EState::Prepared) return;
		FExecutionScope Execution;
		for (auto It = Impl->Participants.rbegin(); It != Impl->Participants.rend(); ++It) (*It)->Abort();
		for (DObject* Object : Impl->Map.GetPreparedObjects()) if (IsValid(Object)) MarkAsGarbage(Object);
		Impl->Pins.clear();
		Impl->State = FImpl::EState::Finished;
		GReplacementActive = false;
	}

	auto FObjectGraphReplacement::Retire() -> bool
	{
		CheckThread();
		if (GReplacementExecuting || Impl->State != FImpl::EState::Committed) return false;
		FExecutionScope Execution;
		for (const auto& P : Impl->Participants) if (!P->CanRetire()) return false;
		for (const auto& Entry : Impl->Map.GetEntries())
			if (Entry.Previous->HasAnyInternalFlags(EObjectInternalFlags::RootSet)
				|| Private::GetStrongObjectReferenceCount(MakeObjectHandle(Entry.Previous)) != 1) return false;
		// Late callbacks cannot turn forced retirement into a dangling strong edge.
		try
		{
			class FRetirementAudit final : public FReferenceCollector
			{
			public:
				const FObjectReplacementMap& Map;
				bool bReferenced = false;
				explicit FRetirementAudit(const FObjectReplacementMap& InMap) : Map(InMap) {}
				auto AddReferencedObject(DObject*& Value) -> void override { bReferenced |= Map.Find(Value) != nullptr; }
			} Audit(Impl->Map);
			FObjectReferenceReplacementPlan Fresh;
			Fresh.Impl->MaximumSlots = Impl->Budget.MaximumReferenceSlots;
			if (!Fresh.Impl->Scan(Impl->Map, Impl->Participants)) return false;
			for (const auto& Edge : Fresh.Impl->Edges) if (Impl->Map.Find(Edge.Value)) return false;
			for (DObject* Owner : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
				if (IsValid(Owner) && !Impl->Map.Find(Owner)) Owner->AddReferencedObjects(Audit);
			if (Audit.bReferenced) return false;
		}
		catch (...) { return false; }
		for (const auto& Entry : Impl->Map.GetEntries()) MarkAsGarbage(Entry.Previous);
		Impl->Pins.clear();
		Impl->State = FImpl::EState::Finished;
		GReplacementActive = false;
		return true;
	}
}
