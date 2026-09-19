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
		using R = EObjectReplacementReason;
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

		auto Fail(E Code, R Reason) -> FObjectReplacementResult
		{
			return {{.Code = Code, .Reason = Reason}};
		}

		auto PropertyContext(FObjectReplacementResult Result, FProperty* P, uint32 Index,
			DObject* Owner = nullptr) -> FObjectReplacementResult
		{
			if (Result) return Result;
			if (Owner) Result.Error.ObjectPath = Owner->GetObjectPath();
			if (P)
			{
				const auto Name = P->NamePrivate.ToString();
				if (Result.Error.PropertyName.empty())
				{
					Result.Error.PropertyName = Name;
					Result.Error.ArrayIndex = Index;
				}
				Result.Error.Route.insert(Result.Error.Route.begin(), std::format("{}[{}]", Name, Index));
			}
			return Result;
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
			return ArePropertyValuesIdentical(P, A, AI, B, BI);
		}

		// Mutates detached storage only. Map keys are copied before modification and
		// inserted into a separate index; no const key is ever modified in place.
		auto RewriteValue(FProperty* P, void* Container, uint32 Index,
			const FObjectReplacementMap& Map) -> FObjectReplacementResult;
		auto Rewrite(FProperty* P, void* Container, uint32 Index,
			const FObjectReplacementMap& Map) -> FObjectReplacementResult
		{
			return PropertyContext(RewriteValue(P, Container, Index, Map), P, Index);
		}
		auto RewriteValue(FProperty* P, void* Container, uint32 Index,
			const FObjectReplacementMap& Map) -> FObjectReplacementResult
		{
			if (!HasReferenceMetadata(P)) return {};
			if (P->GetKind() == K::Object)
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(P);
				if (const auto* Entry = Map.Find(ObjectProperty->GetObjectPropertyValue(Container, Index)))
				{
					if (!Entry->Replacement) return Fail(E::UnmappedReference, R::DetachedUnmappedReference);
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
					return Fail(E::Unsupported, R::ArrayMutableTraversal);
				struct FContext { FProperty* Inner; const FObjectReplacementMap& Map; FObjectReplacementResult Result; }
					Context{Array->GetInner(), Map, {}};
				const auto Result = Array->VisitMutableElements(Container, [](void* Raw, uint64 ElementIndex, void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					C.Result = Rewrite(C.Inner, Value, 0, C.Map);
					if (!C.Result) C.Result.Error.Route.insert(C.Result.Error.Route.begin(), std::to_string(ElementIndex));
					return bool(C.Result);
				}, &Context, Index);
				if (!Context.Result) return Context.Result;
				if (Result != EContainerOpResult::Success) return {{.Code = E::Unsupported, .Reason = R::ArrayTraversal, .Message = std::format("Container operation failed: {}", static_cast<uint32>(Result))}};
			}
			else if (P->GetKind() == K::Map)
			{
				auto* Property = static_cast<FMapProperty*>(P);
				if (!Property->HasMapOps()) return Fail(E::Unsupported, R::MapOperations);
				const auto& Ops = Property->GetOps();
				if (!Ops.CreateDetached || !Ops.DestroyDetached || !Ops.Commit || !Ops.InsertCopy || !Ops.VisitConst)
					return Fail(E::Unsupported, R::MapTransactionalOperations);
				void* Detached = nullptr;
				if (const auto Created = Ops.CreateDetached(&Detached); Created != EContainerOpResult::Success)
					return {{.Code = E::AllocationFailure, .Reason = R::MapAllocation, .Message = std::format("Container allocation failed: {}", static_cast<uint32>(Created))}};
				std::unique_ptr<void, decltype(Ops.DestroyDetached)> Storage(Detached, Ops.DestroyDetached);
				struct FContext { FMapProperty* Property; const FObjectReplacementMap& Map; void* Storage; FObjectReplacementResult Result; }
					Context{Property, Map, Detached, {}};
				const auto Result = Property->VisitEntries(Container, [](void* Raw, const void* Key, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					FReflectedValueStorage KCopy, VCopy;
					auto Copied = KCopy.CopyConstruct(C.Property->GetKeyProp(), C.Property->GetKeyProp()->GetValuePtr(Key));
					if (Copied) Copied = VCopy.CopyConstruct(C.Property->GetValueProp(), C.Property->GetValueProp()->GetValuePtr(Value));
					if (!Copied)
					{
						C.Result = {{.Code = E::Unsupported, .Reason = R::MapValueCopy, .Message = FormatPropertyValueError(Copied.Error)}}; return false;
					}
					C.Result = Rewrite(C.Property->GetKeyProp(), KCopy.GetContainer(), 0, C.Map);
					if (C.Result) C.Result = Rewrite(C.Property->GetValueProp(), VCopy.GetContainer(), 0, C.Map);
					if (!C.Result) return false;
					const auto Insert = C.Property->GetOps().InsertCopy(C.Storage, KCopy.GetValue(), VCopy.GetValue());
					if (Insert != EContainerOpResult::Success)
						C.Result = {{.Code = Insert == EContainerOpResult::DuplicateKey ? E::MapCollision : E::Unsupported,
							.Reason = R::MapInsertion, .Message = std::format("Container insertion failed: {}", static_cast<uint32>(Insert))}};
					return bool(C.Result);
				}, &Context, Index);
				if (!Context.Result) return Context.Result;
				if (Result != EContainerOpResult::Success) return {{.Code = E::Unsupported, .Reason = R::MapTraversal, .Message = std::format("Container operation failed: {}", static_cast<uint32>(Result))}};
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
		const FObjectReplacementBudget& Budget) -> FObjectReplacementMapResult
	{
		CheckThread();
		using R = EObjectReplacementMapReason;
		size_t PackageIndex = 0;
		auto Failure = [&](E Code, R Reason, const DObject* Object = nullptr,
			uint64 ActualCount = 0, uint64 MaximumCount = 0) -> FObjectReplacementMapResult {
			FObjectReplacementMapError Error;
			Error.Code = Code;
			Error.Reason = Reason;
			Error.PackageIndex = PackageIndex;
			Error.ActualCount = ActualCount;
			Error.MaximumCount = MaximumCount;
			if (Object) Error.ObjectPath = Object->GetObjectPath();
			return {std::move(Error)};
		};
		FObjectReplacementMap Candidate;
		if (Packages.empty() || Packages.size() > Budget.MaximumPackages)
			return Failure(E::BudgetExceeded, R::PackageBudget, nullptr, Packages.size(), Budget.MaximumPackages);
		const auto All = GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished);
		std::unordered_set<DPackage*> Seen;
		std::unordered_set<std::string> Paths;
		for (const auto& Pair : Packages)
		{
			if (!IsValid(Pair.Prepared) || Pair.Current == Pair.Prepared
				|| (Pair.Current && (!IsValid(Pair.Current) || !Pair.Current->IsAssetPackage() || Pair.Current->IsGraphPrivate()))
				|| !Pair.Prepared->IsPreparedAssetPackage()
				|| FindPackage(Pair.Prepared->GetPackagePath()) != Pair.Current
				|| (Pair.Current && !Seen.insert(Pair.Current).second) || !Seen.insert(Pair.Prepared).second
				|| !Paths.insert(Pair.Prepared->GetPackagePath()).second)
				return Failure(E::InvalidGraph, R::InvalidPackagePair);
			std::unordered_map<std::vector<FName>, DObject*, FRelativeNamesHash> Old, New;
			for (DObject* Object : All)
			{
				if (Object->GetPackage() != Pair.Prepared && (!Pair.Current || Object->GetPackage() != Pair.Current)) continue;
				if (!IsValid(Object) || Object->IsTemplateObject()) return Failure(E::InvalidGraph, R::InvalidObject, Object);
				auto& Graph = Object->GetPackage() == Pair.Current ? Old : New;
				auto Names = RelativeNames(Object, Object->GetPackage());
				if (!Graph.emplace(std::move(Names), Object).second)
					return Failure(E::InvalidGraph, R::DuplicateIdentity, Object);
				if (Old.size() + New.size() + Candidate.Entries.size() + Candidate.PreparedObjects.size() > Budget.MaximumObjects)
					return Failure(E::BudgetExceeded, R::ObjectBudget, Object,
						Old.size() + New.size() + Candidate.Entries.size() + Candidate.PreparedObjects.size(), Budget.MaximumObjects);
			}
			for (const auto& [Names, Object] : Old)
			{
				const auto It = New.find(Names);
				DObject* Replacement = It == New.end() ? nullptr : It->second;
				if (Replacement && !Replacement->IsA(Object->GetClass()))
				{
					auto Result = Failure(E::IncompatibleType, R::IncompatibleType, Object);
					Result.Error.ExpectedType = Object->GetClass()->GetQualifiedName().ToString();
					Result.Error.ActualType = Replacement->GetClass()->GetQualifiedName().ToString();
					return Result;
				}
				Candidate.Indices.emplace(Object, Candidate.Entries.size());
				Candidate.Entries.push_back({Object, Replacement});
			}
			for (const auto& Item : New) Candidate.PreparedObjects.push_back(Item.second);
			++PackageIndex;
		}
		*this = std::move(Candidate);
		return {};
	}

	auto FormatObjectReplacementMapError(const FObjectReplacementMapError& Error) -> std::string
	{
		switch (Error.Reason)
		{
		case EObjectReplacementMapReason::None: return {};
		case EObjectReplacementMapReason::PackageBudget: return "Replacement package count is empty or exceeds its budget.";
		case EObjectReplacementMapReason::InvalidPackagePair: return "Expected unique live/prepared package pairs at the same path.";
		case EObjectReplacementMapReason::InvalidObject: return "Graph contains a dead/template object.";
		case EObjectReplacementMapReason::DuplicateIdentity: return "Graph contains duplicate relative Outer paths.";
		case EObjectReplacementMapReason::ObjectBudget: return "Replacement graph object budget exceeded.";
		case EObjectReplacementMapReason::IncompatibleType: return "Replacement type is not assignable to the previous type.";
		}
		return {};
	}

	auto FormatObjectReplacementError(const FObjectReplacementError& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.Message;
		switch (Error.Reason)
		{
		case EObjectReplacementReason::None: return {};
		case EObjectReplacementReason::DetachedUnmappedReference: return "Detached value has an unmapped reference.";
		case EObjectReplacementReason::ArrayMutableTraversal: return "Array lacks mutable detached traversal.";
		case EObjectReplacementReason::ArrayTraversal: return "Array traversal failed.";
		case EObjectReplacementReason::MapOperations: return "Map has no operations.";
		case EObjectReplacementReason::MapTransactionalOperations: return "Map lacks transactional detached operations.";
		case EObjectReplacementReason::MapAllocation: return "Could not allocate detached Map.";
		case EObjectReplacementReason::MapValueCopy: return "Map key/value cannot be copied.";
		case EObjectReplacementReason::MapInsertion: return "Replacement Map key collides or insertion failed.";
		case EObjectReplacementReason::MapTraversal: return "Map traversal failed.";
		case EObjectReplacementReason::ReferenceMetadata: return "Reference metadata is missing or exceeds the traversal depth limit.";
		case EObjectReplacementReason::ReferenceBudget: return "Reference slot budget exceeded.";
		case EObjectReplacementReason::ReferenceWriter: return "Reference property has no writer.";
		case EObjectReplacementReason::TemplateReference: return "Templates cannot reference a replaced live graph.";
		case EObjectReplacementReason::ExternalUnmappedReference: return "A graph-external reference has no replacement.";
		case EObjectReplacementReason::ReferenceType: return "Replacement is incompatible with a reference property.";
		case EObjectReplacementReason::StructMetadata: return "Struct metadata is missing.";
		case EObjectReplacementReason::NativeStructAdapter: return "A reference-bearing native struct needs a dedicated replacement adapter.";
		case EObjectReplacementReason::ArrayReferenceMetadata: return "Array lacks reference traversal metadata.";
		case EObjectReplacementReason::MapReferenceMetadata: return "Map lacks reference traversal metadata.";
		case EObjectReplacementReason::ReferenceTraversal: return "Reference traversal failed.";
		case EObjectReplacementReason::ContainerCommit: return "Container has no nonthrowing commit adapter.";
		case EObjectReplacementReason::ContainerCopy: return "Reference container cannot be copied for atomic publication.";
		case EObjectReplacementReason::ContainerComparison: return "Reference container cannot be compared for stale validation.";
		case EObjectReplacementReason::NativeOwnerParticipant: return "An enumerated native owner has no replacement participant.";
		case EObjectReplacementReason::AlreadyActive: return "A replacement is already active.";
		case EObjectReplacementReason::PackageReservation: return "New package path is already reserved.";
		case EObjectReplacementReason::InvalidParticipant: return "Duplicate/null participant.";
		case EObjectReplacementReason::RootedObject: return "An explicitly rooted old object cannot be retired.";
		case EObjectReplacementReason::StrongOwnerClaim: return "An external strong owner has no exact replacement claim.";
		case EObjectReplacementReason::PrepareAllocation: return "Replacement preparation allocation failed.";
		case EObjectReplacementReason::PrepareException: return "Replacement preparation callback threw.";
		case EObjectReplacementReason::NotPrepared: return "Replacement is not prepared or is executing.";
		case EObjectReplacementReason::ObjectMembershipChanged: return "Object membership changed during preparation.";
		case EObjectReplacementReason::ObjectIdentityChanged: return "Graph identity changed.";
		case EObjectReplacementReason::ObjectRootChanged: return "An old object acquired a manual root during preparation.";
		case EObjectReplacementReason::PackageChanged: return "Package registration or edit revision changed.";
		case EObjectReplacementReason::ParticipantChanged: return "Native participant changed.";
		case EObjectReplacementReason::StrongOwnerChanged: return "Strong ownership changed.";
		case EObjectReplacementReason::ReferenceSlotsChanged: return "Reference owners or slots changed.";
		case EObjectReplacementReason::ContainerChanged: return "Reference container contents changed.";
		case EObjectReplacementReason::ValidationMutation: return "Validation changed object membership or ownership.";
		case EObjectReplacementReason::ValidateAllocation: return "Replacement validation allocation failed.";
		case EObjectReplacementReason::ValidateException: return "Replacement validation callback threw.";
		case EObjectReplacementReason::ReplacementMap:
			if (!Error.Message.empty()) return Error.Message;
			return "Replacement map construction failed.";
		case EObjectReplacementReason::ParticipantBusy: return "Replacement participant is busy.";
		case EObjectReplacementReason::ParticipantUnmappedPackage: return "Replacement participant could not map a package.";
		case EObjectReplacementReason::ParticipantRejected: return "Replacement participant rejected preparation.";
		case EObjectReplacementReason::PersistenceRejected: return "Replacement persistence failed.";
		}
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
			return PropertyContext(ScanPropertyValue(Owner, P, Container, Index, Map,
				bDetachedAncestor, Changed, Depth), P, Index, Owner);
		}
		auto ScanPropertyValue(DObject* Owner, FProperty* P, void* Container, uint32 Index,
			const FObjectReplacementMap& Map, bool bDetachedAncestor, bool& Changed, uint32 Depth) -> FObjectReplacementResult
		{
			if (!P || Depth > 64) return {{.Code = E::Unsupported, .Reason = R::ReferenceMetadata,
				.ActualCount = Depth, .MaximumCount = 64}};
			if (!HasReferenceMetadata(P)) return {};
			if (P->GetKind() == K::Object)
			{
				auto* Property = static_cast<FObjectProperty*>(P);
				DObject* Value = Property->GetObjectPropertyValue(Container, Index);
				if (Edges.size() >= MaximumSlots) return {{.Code = E::BudgetExceeded, .Reason = R::ReferenceBudget,
					.ActualCount = Edges.size() + 1, .MaximumCount = MaximumSlots}};
				Edges.push_back({Owner, P, P->GetValuePtr(Container, Index), Value});
				if (const auto* Entry = Map.Find(Value))
				{
					if (!Property->HasObjectValueWriter()) return Fail(E::Unsupported, R::ReferenceWriter);
					if (Owner->IsTemplateObject()) return Fail(E::Unsupported, R::TemplateReference);
					if (!Entry->Replacement) return Fail(E::UnmappedReference, R::ExternalUnmappedReference);
					if (P->GetReferencedClass() && !Entry->Replacement->IsA(P->GetReferencedClass()))
						return {{.Code = E::IncompatibleType, .Reason = R::ReferenceType,
							.ExpectedType = P->GetReferencedClass()->GetQualifiedName().ToString(),
							.ActualType = Entry->Replacement->GetClass()->GetQualifiedName().ToString()}};
					Changed = true;
					if (!bDetachedAncestor) Slots.push_back({Property, Container, Index, Entry->Replacement});
				}
				return {};
			}
			if (P->GetKind() == K::Struct)
			{
				auto* Type = static_cast<FStructProperty*>(P)->GetStruct();
				if (!Type) return Fail(E::Unsupported, R::StructMetadata);
				FObjectReplacementResult Result;
				bool StructChanged = false;
				Type->ForEachProperty([&](FProperty* Field) {
					for (uint32 I = 0; Result && I < Field->GetArrayDim(); ++I)
						Result = ScanProperty(Owner, Field, P->GetValuePtr(Container, Index), I, Map, bDetachedAncestor, StructChanged, Depth + 1);
				});
				Changed |= StructChanged;
				if (!Result) return Result;
				if (StructChanged && Type->HasReferenceCollector())
					return Fail(E::Unsupported, R::NativeStructAdapter);
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
					return Fail(E::Unsupported, R::ArrayReferenceMetadata);
				Traversal = Array->VisitElements(Container, [](void* Raw, uint64 ElementIndex, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					C.Result = C.Plan.ScanProperty(C.Owner, static_cast<FArrayProperty*>(C.P)->GetInner(),
						const_cast<void*>(Value), 0, C.Map, true, C.Changed, C.Depth + 1);
					if (!C.Result) C.Result.Error.Route.insert(C.Result.Error.Route.begin(), std::to_string(ElementIndex));
					return bool(C.Result);
				}, &Context, Index);
			}
			else
			{
				auto* Property = static_cast<FMapProperty*>(P);
				if (!Property->HasMapOps() || !Property->GetKeyProp() || !Property->GetValueProp() || !Property->GetOps().VisitConst)
					return Fail(E::Unsupported, R::MapReferenceMetadata);
				Traversal = Property->VisitEntries(Container, [](void* Raw, const void* Key, const void* Value) {
					auto& C = *static_cast<FContext*>(Raw);
					auto* M = static_cast<FMapProperty*>(C.P);
					C.Result = C.Plan.ScanProperty(C.Owner, M->GetKeyProp(), const_cast<void*>(Key), 0, C.Map, true, C.Changed, C.Depth + 1);
					if (C.Result) C.Result = C.Plan.ScanProperty(C.Owner, M->GetValueProp(), const_cast<void*>(Value), 0, C.Map, true, C.Changed, C.Depth + 1);
					return bool(C.Result);
				}, &Context, Index);
			}
			if (!Context.Result) return Context.Result;
			if (Traversal != EContainerOpResult::Success) return {{.Code = E::Unsupported, .Reason = R::ReferenceTraversal, .Message = std::format("Reference traversal failed: {}", static_cast<uint32>(Traversal))}};
			Changed |= LocalChanged;
			if (!LocalChanged || bDetachedAncestor) return {};
			const bool bCanCommit = P->GetKind() == K::Array
				? static_cast<FArrayProperty*>(P)->HasCapability(EArrayOpsFlags::TransactionalCommit)
				: static_cast<FMapProperty*>(P)->HasCapability(EMapOpsFlags::TransactionalCommit);
			if (!bCanCommit) return Fail(E::Unsupported, R::ContainerCommit);
			FContainerWrite Write{P, Container, Index, {}, {}};
			auto Copied = Write.Before.CopyConstruct(P, P->GetValuePtr(Container, Index), Index);
			if (Copied) Copied = Write.After.CopyConstruct(P, P->GetValuePtr(Container, Index), Index);
			if (!Copied) return {{.Code = E::Unsupported, .Reason = R::ContainerCopy, .Message = FormatPropertyValueError(Copied.Error)}};
			if (!Equal(P, Container, Index, Write.Before.GetContainer(), Index))
				return Fail(E::Unsupported, R::ContainerComparison);
			auto Result = RewriteValue(P, Write.After.GetContainer(), Index, Map);
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
					return FObjectReplacementResult{{.Code = E::Unsupported, .Reason = R::NativeOwnerParticipant, .ObjectPath = Owner->GetObjectPath()}};
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
		std::vector<std::pair<FObjectKey, std::string>> Identities;
		std::vector<uint64> PackageRevisions;
		uint64 ArrayRevision = 0;

		auto CheckStrongOwners(E Code, R Reason) const -> FObjectReplacementResult
		{
			for (const auto& Entry : Map.GetEntries())
			{
				uint64 Claimed = 1; // This operation pins every live object once.
				for (const auto& P : Participants) Claimed += P->GetStrongReferenceCount(*Entry.Previous);
				const uint64 Actual = Private::GetStrongObjectReferenceCount(FObjectKey(Entry.Previous));
				if (Actual != Claimed) return {{.Code = Code, .Reason = Reason,
					.ObjectPath = Entry.Previous->GetObjectPath(), .ActualCount = Actual, .ExpectedCount = Claimed}};
			}
			return {};
		}
	};

	FObjectGraphReplacement::FObjectGraphReplacement() : Impl(std::make_unique<FImpl>()) {}
	FObjectGraphReplacement::~FObjectGraphReplacement()
	{
		require(Impl->State != FImpl::EState::Committed);
		Abort();
	}

	auto FObjectGraphReplacement::GetMap() const -> const FObjectReplacementMap& { return Impl->Map; }
	auto FObjectGraphReplacement::OwnsPreparedPackage(const DPackage& Package) const -> bool
	{
		return Impl->State == FImpl::EState::Prepared && Package.IsPreparedAssetPackage() &&
			std::ranges::any_of(Impl->Packages, [&](const auto& Pair) { return Pair.Prepared == &Package; });
	}

	auto FObjectGraphReplacement::Prepare(std::span<const FObjectReplacementPackagePair> Packages,
		std::span<const std::shared_ptr<IObjectReplacementParticipant>> Participants,
		const FObjectReplacementBudget& Budget) -> FObjectReplacementResult
	{
		CheckThread();
		if (GReplacementActive || Impl->State != FImpl::EState::Empty) return Fail(E::Busy, R::AlreadyActive);
		FExecutionScope Execution;
		GReplacementActive = true;
		size_t PreparedParticipants = 0;
		auto Reject = [&](FObjectReplacementResult Result) {
			for (const auto& Pair : Impl->Packages) if (!Pair.Current) Pair.Prepared->ReleasePreparedPackageRegistration();
			while (PreparedParticipants) Impl->Participants[--PreparedParticipants]->Abort();
			Impl->Pins.clear();
			Impl->Participants.clear();
			Impl->State = FImpl::EState::Finished;
			GReplacementActive = false;
			return Result;
		};
		try
		{
			const auto MapResult = Impl->Map.Build(Packages, Budget);
			if (!MapResult) return Reject({{.Code = MapResult.Error.Code,
				.Reason = R::ReplacementMap, .Message = FormatObjectReplacementMapError(MapResult.Error)}});
			FObjectReplacementResult Result;
			Impl->Budget = Budget;
			Impl->Packages.assign(Packages.begin(), Packages.end());
			for (const auto& Pair : Packages)
				if (!Pair.Current && !Pair.Prepared->ReservePreparedPackageRegistration())
					return Reject(FObjectReplacementResult{{.Code = E::InvalidGraph, .Reason = R::PackageReservation, .ObjectPath = Pair.Prepared->GetPackagePath()}});
			Impl->Participants.assign(Participants.begin(), Participants.end());
			std::unordered_set<const IObjectReplacementParticipant*> Unique;
			for (const auto& P : Impl->Participants)
				if (!P || !Unique.insert(P.get()).second) return Reject(Fail(E::InvalidGraph, R::InvalidParticipant));
			for (const auto& Entry : Impl->Map.GetEntries())
				if (Entry.Previous->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
					return Reject(FObjectReplacementResult{{.Code = E::Unsupported, .Reason = R::RootedObject, .ObjectPath = Entry.Previous->GetObjectPath()}});
			const auto PreparedObjects = Impl->Map.GetPreparedObjects();
			const std::unordered_set<DObject*> PreparedSet(PreparedObjects.begin(), PreparedObjects.end());
			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (!IsValid(Object)) continue;
				Impl->Pins.emplace_back(Object);
				if (Impl->Map.Find(Object) || PreparedSet.contains(Object))
					Impl->Identities.emplace_back(FObjectKey(Object), Object->GetObjectPath());
			}
			for (const auto& Pair : Packages)
			{
				Impl->PackageRevisions.push_back(Pair.Current ? Pair.Current->GetEditRevision() : 0);
				Impl->PackageRevisions.push_back(Pair.Prepared->GetEditRevision());
			}
			for (const auto& P : Impl->Participants)
			{
				++PreparedParticipants;
				Result = P->Prepare(Impl->Map);
				if (!Result)
				{
					Result.Error.ParticipantIndex = PreparedParticipants - 1;
					return Reject(std::move(Result));
				}
			}
			if (auto Owners = Impl->CheckStrongOwners(E::Unsupported, R::StrongOwnerClaim); !Owners) return Reject(std::move(Owners));
			Impl->References.Impl->MaximumSlots = Budget.MaximumReferenceSlots;
			Result = Impl->References.Impl->Scan(Impl->Map, Impl->Participants);
			if (!Result) return Reject(std::move(Result));
			Impl->ArrayRevision = GDObjectArray.GetRevision();
			Impl->State = FImpl::EState::Prepared;
			return {};
		}
		catch (const std::bad_alloc&) { return Reject(Fail(E::AllocationFailure, R::PrepareAllocation)); }
		catch (...) { return Reject(Fail(E::ParticipantRejected, R::PrepareException)); }
	}

	auto FObjectGraphReplacement::TryCommit(const std::function<FObjectReplacementResult()>& Persist) -> FObjectReplacementResult
	{
		CheckThread();
		if (GReplacementExecuting || Impl->State != FImpl::EState::Prepared) return Fail(E::Busy, R::NotPrepared);
		FExecutionScope Execution;
		try
		{
			if (GDObjectArray.GetRevision() != Impl->ArrayRevision) return {{.Code = E::Stale, .Reason = R::ObjectMembershipChanged,
				.ActualRevision = GDObjectArray.GetRevision(), .ExpectedRevision = Impl->ArrayRevision}};
			for (const auto& [Handle, Path] : Impl->Identities)
			{
				DObject* Object = ResolveObjectKey(Handle);
				if (!IsValid(Object) || Object->GetObjectPath() != Path) return FObjectReplacementResult{{.Code = E::Stale, .Reason = R::ObjectIdentityChanged, .ObjectPath = Path}};
			}
			for (const auto& Entry : Impl->Map.GetEntries())
				if (Entry.Previous->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
					return FObjectReplacementResult{{.Code = E::Stale, .Reason = R::ObjectRootChanged, .ObjectPath = Entry.Previous->GetObjectPath()}};
			for (size_t I = 0; I < Impl->Packages.size(); ++I)
			{
				const auto& Pair = Impl->Packages[I];
				if (FindPackage(Pair.Prepared->GetPackagePath()) != Pair.Current
					|| !Pair.Prepared->IsPreparedAssetPackage()
					|| (Pair.Current && Pair.Current->GetEditRevision() != Impl->PackageRevisions[I * 2])
					|| Pair.Prepared->GetEditRevision() != Impl->PackageRevisions[I * 2 + 1])
					return FObjectReplacementResult{{.Code = E::Stale, .Reason = R::PackageChanged, .ObjectPath = Pair.Prepared->GetPackagePath()}};
			}
			for (size_t I = 0; I < Impl->Participants.size(); ++I)
				if (!Impl->Participants[I]->Validate()) return {{.Code = E::Stale,
					.Reason = R::ParticipantChanged, .ParticipantIndex = I}};
			if (auto Owners = Impl->CheckStrongOwners(E::Stale, R::StrongOwnerChanged); !Owners) return Owners;
			FObjectReferenceReplacementPlan Fresh;
			Fresh.Impl->MaximumSlots = Impl->Budget.MaximumReferenceSlots;
			auto Result = Fresh.Impl->Scan(Impl->Map, Impl->Participants);
			if (!Result) return Result;
			if (Fresh.Impl->Edges != Impl->References.Impl->Edges) return Fail(E::Stale, R::ReferenceSlotsChanged);
			for (const auto& Write : Impl->References.Impl->Containers)
				if (!Equal(Write.Property, Write.Container, Write.Index, Write.Before.GetContainer(), Write.Index))
					return PropertyContext(Fail(E::Stale, R::ContainerChanged), Write.Property, Write.Index);
			if (GDObjectArray.GetRevision() != Impl->ArrayRevision)
				return {{.Code = E::Stale, .Reason = R::ValidationMutation,
					.ActualRevision = GDObjectArray.GetRevision(), .ExpectedRevision = Impl->ArrayRevision}};
			if (auto Owners = Impl->CheckStrongOwners(E::Stale, R::ValidationMutation); !Owners) return Owners;
			// All fallible traversal, copies, collision checks and participant validation precede this call.
			if (Persist)
			{
				const auto Persisted = Persist();
				if (!Persisted) return Persisted;
			}
			CommitPrepared();
			return {};
		}
		catch (const std::bad_alloc&) { return Fail(E::AllocationFailure, R::ValidateAllocation); }
		catch (...) { return Fail(E::ParticipantRejected, R::ValidateException); }
	}

	auto FObjectGraphReplacement::CommitPrepared() noexcept -> void
	{
		for (const auto& Pair : Impl->Packages)
		{
			Pair.Prepared->SetStandaloneResidency(!Pair.Current || Pair.Current->HasAnyObjectFlags(EObjectFlags::Standalone));
			if (Pair.Current) Pair.Current->SetStandaloneResidency(false);
			Pair.Prepared->CommitPreparedPackageRegistration(Pair.Current);
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
		for (const auto& Pair : Impl->Packages) if (!Pair.Current) Pair.Prepared->ReleasePreparedPackageRegistration();
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
				|| Private::GetStrongObjectReferenceCount(FObjectKey(Entry.Previous)) != 1) return false;
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
