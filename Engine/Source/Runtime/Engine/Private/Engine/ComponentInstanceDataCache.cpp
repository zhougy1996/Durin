#include "Engine/ComponentInstanceDataCache.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	namespace
	{
		auto InstanceDataTypes() -> std::unordered_map<std::string, const FComponentInstanceDataType*>&
		{
			static std::unordered_map<std::string, const FComponentInstanceDataType*> Types;
			return Types;
		}
		auto InstanceDataTypesMutex() -> std::mutex&
		{
			static std::mutex Mutex;
			return Mutex;
		}
		auto SerializeReference(FArchive& Archive, FPersistentObjectRef& Reference) -> void
		{
			auto Key = Reference.GetKey(); Key.SerializeForSnapshot(Archive);
			if (Archive.IsLoading() && !Archive.IsError()) Reference = FPersistentObjectRef::FromKey(Key);
		}
		auto SerializeTransform(FArchive& Ar, FTransform& Value) -> void
		{
			Ar << Value.Rotation.x << Value.Rotation.y << Value.Rotation.z << Value.Rotation.w;
			Ar << Value.Translation.x << Value.Translation.y << Value.Translation.z;
			Ar << Value.Scale3D.x << Value.Scale3D.y << Value.Scale3D.z;
		}
		template<typename T>
		auto SerializeEntries(FArchive& Ar, std::vector<T>& Entries, auto Serialize) -> void
		{
			uint64 Count = Entries.size(); Ar << Count;
			if (Count > 1000000) { Ar.SetError("Transaction annotation exceeds the component count limit."); return; }
			if (Ar.IsLoading() && !Ar.IsError()) Entries.resize(static_cast<size_t>(Count));
			for (auto& Entry : Entries) { if (Ar.IsError()) break; Serialize(Entry); }
		}
	}

	FActorComponentInstanceSourceInfo::FActorComponentInstanceSourceInfo(const DActorComponent* Component)
	{
		if (!Component) return;
		DObjectForceRegistration(Component->GetClass());
		if (!IsValid(Component->GetClass())) throw std::runtime_error("Transaction component class metadata is unavailable.");
		Class = FPersistentObjectRef(Component->GetClass());
		Owner = FPersistentObjectRef(Component->GetOwner());
		if (Component->GetCreationMethod() == EComponentCreationMethod::Generated)
		{
			auto* Owner = Component->GetOwner();
			if (!Owner || !Owner->GetGeneratedComponentKey(Component, Key))
				throw std::runtime_error("Transaction annotation cannot capture an unpublished generated component.");
			GeneratedOwner = FPersistentObjectRef(Owner);
		}
		else Exact = FPersistentObjectRef(const_cast<DActorComponent*>(Component));
	}
	auto FActorComponentInstanceSourceInfo::Resolve() const -> DActorComponent*
	{
		auto* Component = Cast<DActorComponent>(Exact.Resolve());
		if (!GeneratedOwner.IsNull())
			if (auto* Owner = Cast<AActor>(GeneratedOwner.Resolve())) Component = Owner->FindGeneratedComponent(Key);
		return IsValid(Component) && Class.Resolve() == Component->GetClass() && Owner.Resolve() == Component->GetOwner() ? Component : nullptr;
	}
	auto FActorComponentInstanceSourceInfo::Validate() const -> FTransactionCaptureResult
	{
		if (IsNull())
		{
			if (!Class.IsNull() || !Owner.IsNull() || !Key.Namespace.IsNone() || Key.Id.IsValid())
				return std::unexpected("An empty component source contains incomplete identity metadata.");
			return {};
		}
		if (Owner.IsStale() || Class.IsNull() || Class.IsStale() || Exact.IsStale() || GeneratedOwner.IsStale())
			return std::unexpected("Transaction component annotation dependency expired.");
		if (!GeneratedOwner.IsNull())
		{
			auto* Owner = Cast<AActor>(GeneratedOwner.Resolve());
			if (!Owner || !Key.IsValid()) return std::unexpected("Transaction generated component owner or key is invalid.");
			if (!Exact.IsNull() || this->Owner.Resolve() != Owner)
				return std::unexpected("Generated component source has ambiguous owner identity.");
			if (auto* Component = Owner->FindGeneratedComponent(Key); Component && Component->GetClass() != Class.Resolve())
				return std::unexpected("Transaction generated component exact class changed.");
		}
		else if (!Resolve()) return std::unexpected("Transaction authored component identity changed.");
		return {};
	}
	auto FActorComponentInstanceSourceInfo::MatchesGenerated(const AActor& Owner, const FActorGeneratedComponentKey& InKey) const -> bool
	{ return GeneratedOwner.Resolve() == &Owner && Key == InKey; }
	auto FActorComponentInstanceSourceInfo::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{ Exact.AddReferencedObjects(Collector); GeneratedOwner.AddReferencedObjects(Collector); Class.AddReferencedObjects(Collector); Owner.AddReferencedObjects(Collector); }
	auto FActorComponentInstanceSourceInfo::Serialize(FArchive& Ar) -> void
	{
		SerializeReference(Ar, Exact); SerializeReference(Ar, GeneratedOwner); SerializeReference(Ar, Class); SerializeReference(Ar, Owner);
		Ar << Key.Namespace << Key.Id.A << Key.Id.B << Key.Id.C << Key.Id.D;
		if (Ar.IsLoading() && (!GeneratedOwner.IsNull() && (!Exact.IsNull() || !Key.IsValid())))
			Ar.SetError("Invalid generated component annotation endpoint.");
		if (Ar.IsLoading() && !Ar.IsError())
			if (auto Valid = Validate(); !Valid) Ar.SetError(Valid.error());
	}

	FActorRootComponentReconstructionData::FActorRootComponentReconstructionData(const AActor& Actor, const FActorRootComponentReconstructionData* Previous)
		: SourceActor(const_cast<AActor*>(&Actor))
	{
		auto* RootComponent = Actor.GetRootComponent();
		Root = FActorComponentInstanceSourceInfo(RootComponent);
		bCachedGeneratedRoot = RootComponent && RootComponent->GetCreationMethod() == EComponentCreationMethod::Generated;
		if (bCachedGeneratedRoot)
		{
			RootParent = FActorComponentInstanceSourceInfo(RootComponent->GetAttachParent());
			RootRelative = RootComponent->GetRelativeTransform(); RootWorld = RootComponent->GetWorldTransform();
		}
		auto CaptureChild = [&](DSceneComponent& Child) {
			const auto Endpoint = FActorComponentInstanceSourceInfo(&Child);
			if (std::ranges::any_of(Attached, [&](const auto& Entry) { return Entry.Target == Endpoint; })) return;
			Attached.push_back({Endpoint, FActorComponentInstanceSourceInfo(Child.GetAttachParent()), Child.GetRelativeTransform()});
		};
		for (const auto& Component : Actor.GetComponentsSnapshot())
			if (auto* Generated = Cast<DSceneComponent>(Component.Get()); Generated && Generated->GetCreationMethod() == EComponentCreationMethod::Generated)
				for (const auto& Child : Generated->GetAttachChildren())
					if (Child && Child->GetCreationMethod() != EComponentCreationMethod::Generated) CaptureChild(*Child);
		if (Previous)
			for (const auto& Entry : Previous->Attached)
			{
				auto* Child = Cast<DSceneComponent>(Entry.Target.Resolve());
				if (!Child) throw std::runtime_error("Transaction attachment participant expired during snapshot refresh.");
				CaptureChild(*Child);
			}
	}
	auto FActorRootComponentReconstructionData::Validate(const AActor* ExpectedActor) const -> FTransactionCaptureResult
	{
		if (SourceActor.IsStale() || (ExpectedActor && SourceActor.Resolve() != ExpectedActor)) return std::unexpected("Root reconstruction owner is unavailable or incompatible.");
		for (const auto* Endpoint : {&Root, &RootParent}) if (auto Result = Endpoint->Validate(); !Result) return Result;
		for (const auto& Child : Attached)
		{
			if (auto Result = Child.Target.Validate(); !Result) return Result;
			if (auto Result = Child.Parent.Validate(); !Result) return Result;
		}
		return {};
	}
	auto FActorRootComponentReconstructionData::ApplyToActor(AActor& Actor, bool bRequireRoot) const -> FCommandChangeResult
	{
		if (auto Result = Validate(); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		if (SourceActor.Resolve() != &Actor) return std::unexpected(FCommandChangeError{"Root reconstruction owner mismatch."});
		auto* SceneRoot = Cast<DSceneComponent>(Root.Resolve());
		auto* Parent = Cast<DSceneComponent>(RootParent.Resolve());
		if (bCachedGeneratedRoot && SceneRoot) Actor.SetRootComponent(SceneRoot);
		if (!bRequireRoot && ((!Root.IsNull() && !SceneRoot) || Actor.GetRootComponent() != SceneRoot)) return {};
		if ((!Root.IsNull() && !SceneRoot) || (SceneRoot && SceneRoot->GetOwner() != &Actor))
			return std::unexpected(FCommandChangeError{"Actor transaction root identity is unavailable."});
		if (bCachedGeneratedRoot)
		{
			if (!SceneRoot || SceneRoot->GetOwner() != &Actor || (!RootParent.IsNull() && !Parent)
				|| (Parent && (Parent == SceneRoot || Parent->IsAttachedTo(SceneRoot))))
				return std::unexpected(FCommandChangeError{"Actor generated-root attachment snapshot cannot resolve safely."});
		}
		for (const auto& Child : Attached)
		{
			auto* Component = Cast<DSceneComponent>(Child.Target.Resolve());
			auto* ParentComponent = Cast<DSceneComponent>(Child.Parent.Resolve());
			if (!Component || (!Child.Parent.IsNull() && !ParentComponent)
				|| (ParentComponent && (ParentComponent == Component || ParentComponent->IsAttachedTo(Component))))
				return std::unexpected(FCommandChangeError{"Actor attachment recovery participant cannot resolve safely."});
		}
		FGarbageCollectionDeferralScope DeferCollection;
		FScopedPackageDirtySuppression SuppressDirty;
		if (!Actor.SetRootComponent(SceneRoot)) return std::unexpected(FCommandChangeError{"Actor transaction root could not be reconciled.", true});
		if (bCachedGeneratedRoot)
		{
			const bool AttachedRoot = SceneRoot->GetAttachParent() == Parent || (Parent ? SceneRoot->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative)
				: SceneRoot->DetachFromComponent(EDetachmentTransformRule::KeepRelative));
			if (!AttachedRoot) return std::unexpected(FCommandChangeError{"Actor generated-root attachment restore failed.", true});
			SceneRoot->SetRelativeTransform(RootRelative);
		}
		for (const auto& Child : Attached)
		{
			auto* Component = Cast<DSceneComponent>(Child.Target.Resolve());
			auto* ParentComponent = Cast<DSceneComponent>(Child.Parent.Resolve());
			if (!Component) return std::unexpected(FCommandChangeError{"Actor attachment recovery target expired.", true});
			const bool Restored = Component->GetAttachParent() == ParentComponent || (ParentComponent
				? Component->AttachToComponent(ParentComponent, EAttachmentTransformRule::KeepRelative)
				: Component->DetachFromComponent(EDetachmentTransformRule::KeepRelative));
			if (!Restored) return std::unexpected(FCommandChangeError{"Actor attachment recovery failed.", true});
			Component->SetRelativeTransform(Child.Relative);
		}
		return {};
	}
	auto FActorRootComponentReconstructionData::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		SourceActor.AddReferencedObjects(Collector); Root.AddReferencedObjects(Collector); RootParent.AddReferencedObjects(Collector);
		for (const auto& Child : Attached) { Child.Target.AddReferencedObjects(Collector); Child.Parent.AddReferencedObjects(Collector); }
	}
	auto FActorRootComponentReconstructionData::Serialize(FArchive& Ar) -> void
	{
		SerializeReference(Ar, SourceActor); Ar << bCachedGeneratedRoot; Root.Serialize(Ar); RootParent.Serialize(Ar); SerializeTransform(Ar, RootRelative); SerializeTransform(Ar, RootWorld);
		SerializeEntries(Ar, Attached, [&](auto& Value) { Value.Target.Serialize(Ar); Value.Parent.Serialize(Ar); SerializeTransform(Ar, Value.Relative); });
	}
	auto FActorRootComponentReconstructionData::GetAllocatedSize() const -> size_t
	{
		constexpr auto Limit = std::numeric_limits<size_t>::max();
		if (Attached.capacity() > (Limit - sizeof(*this)) / sizeof(FAttachedComponent)) return Limit;
		return sizeof(*this) + Attached.capacity() * sizeof(FAttachedComponent);
	}

	auto FActorComponentInstanceSourceInfo::MatchesComponent(const DActorComponent& Component, const FActorGeneratedComponentKey* CandidateKey) const -> bool
	{
		if (!IsValid(&Component) || Component.GetClass() != Class.Resolve() || Component.GetOwner() != Owner.Resolve()) return false;
		if (!GeneratedOwner.IsNull())
		{
			FActorGeneratedComponentKey LiveKey;
			if (!CandidateKey && Component.GetOwner() && Component.GetOwner()->GetGeneratedComponentKey(&Component, LiveKey)) CandidateKey = &LiveKey;
			return CandidateKey && Component.GetCreationMethod() == EComponentCreationMethod::Generated
				&& GeneratedOwner.Resolve() == Component.GetOwner() && *CandidateKey == Key;
		}
		return Exact.Resolve() == &Component;
	}

	auto FActorComponentInstanceData::RegisterDataType(const FComponentInstanceDataType& Type) -> void
	{
		if (Type.Name.empty()) throw std::logic_error("Component instance data requires a stable type name.");
		std::lock_guard Lock(InstanceDataTypesMutex());
		const auto [Entry, Inserted] = InstanceDataTypes().emplace(Type.Name, &Type);
		if (!Inserted && Entry->second != &Type) throw std::logic_error("Duplicate component instance data type name.");
	}
	auto FActorComponentInstanceData::GetDataType() const -> const FComponentInstanceDataType&
	{ return RegisterType<FActorComponentInstanceData>("Durin.ActorComponentInstanceData"); }
	auto FSceneComponentInstanceData::GetDataType() const -> const FComponentInstanceDataType&
	{ return RegisterType<FSceneComponentInstanceData>("Durin.SceneComponentInstanceData"); }
	auto FActorComponentInstanceData::FindDataType(std::string_view Name) -> const FComponentInstanceDataType*
	{
		RegisterType<FActorComponentInstanceData>("Durin.ActorComponentInstanceData");
		RegisterType<FSceneComponentInstanceData>("Durin.SceneComponentInstanceData");
		std::lock_guard Lock(InstanceDataTypesMutex());
		const auto Entry = InstanceDataTypes().find(std::string(Name));
		return Entry == InstanceDataTypes().end() ? nullptr : Entry->second;
	}

	FActorComponentInstanceData::FActorComponentInstanceData(const DActorComponent& Component) : Source(&Component) {}
	auto FActorComponentInstanceData::MatchesComponent(const DActorComponent& Component, const FActorGeneratedComponentKey* Key) const -> bool
	{ return Source.MatchesComponent(Component, Key); }
	auto FActorComponentInstanceData::ApplyToComponent(DActorComponent&, ECacheApplyPhase) const -> FCommandChangeResult { return {}; }
	auto FActorComponentInstanceData::AddReferencedObjects(FReferenceCollector& Collector) const -> void { Source.AddReferencedObjects(Collector); }
	auto FActorComponentInstanceData::Serialize(FArchive& Archive) -> void { Source.Serialize(Archive); }

	FSceneComponentInstanceData::FSceneComponentInstanceData(const DActorComponent& Component)
		: FActorComponentInstanceData(Component), Relative(static_cast<const DSceneComponent&>(Component).GetRelativeTransform()) {}
	auto FSceneComponentInstanceData::ApplyToComponent(DActorComponent& Component, ECacheApplyPhase Phase) const -> FCommandChangeResult
	{
		if (Phase != ECacheApplyPhase::PostUserConstructionScript) return {};
		auto* Scene = Cast<DSceneComponent>(&Component);
		if (!Scene) return std::unexpected(FCommandChangeError{"Scene instance data requires the same scene component class."});
		if (!Scene->IsTransformModifiedByConstructionScript()) Scene->SetRelativeTransform(Relative);
		return {};
	}
	auto FSceneComponentInstanceData::Serialize(FArchive& Archive) -> void
	{ FActorComponentInstanceData::Serialize(Archive); SerializeTransform(Archive, Relative); }

	FComponentInstanceDataCache::FComponentInstanceDataCache(const AActor& Actor)
		: SourceActor(const_cast<AActor*>(&Actor))
	{
		for (const auto& Component : Actor.GetComponentsSnapshot())
			if (Component)
			{
				if (auto Data = Component->GetComponentInstanceData(); Data && Data->ContainsData())
				{
					if (!Data->MatchesComponent(*Component)) throw std::runtime_error("Component cache hook returned incompatible source data.");
					ComponentsInstanceData.push_back(std::move(Data));
				}
			}
	}
	auto FComponentInstanceDataCache::HasInstanceData() const -> bool
	{
		return !ComponentsInstanceData.empty();
	}
	auto FComponentInstanceDataCache::Validate(const AActor* ExpectedActor) const -> FTransactionCaptureResult
	{
		if (SourceActor.IsStale() || (ExpectedActor && SourceActor.Resolve() != ExpectedActor)) return std::unexpected("Component instance cache owner is unavailable or incompatible.");
		for (const auto& Data : ComponentsInstanceData)
		{
			if (!Data) return std::unexpected("Null component instance cache entry.");
			if (auto Result = Data->GetSourceInfo().Validate(); !Result) return Result;
		}
		return {};
	}
	auto FComponentInstanceDataCache::ValidateComponent(const DActorComponent& Component,
		const FActorGeneratedComponentKey* CandidateKey) const -> FTransactionCaptureResult
	{
		if (!IsValid(&Component) || Component.IsBeingDestroyed()) return std::unexpected("Component instance cache candidate is retired.");
		if (SourceActor.Resolve() != Component.GetOwner()) return std::unexpected("Component instance cache owner mismatch.");
		if (CandidateKey && Component.GetOwner())
		{
			auto Check = [&](const FActorComponentInstanceSourceInfo& Source) {
				return !Source.MatchesGenerated(*Component.GetOwner(), *CandidateKey) || Source.MatchesComponent(Component, CandidateKey);
			};
			for (const auto& Data : ComponentsInstanceData)
				if (Data && !Check(Data->GetSourceInfo())) return std::unexpected("Component instance cache exact class mismatch.");
		}
		return {};
	}

	auto FComponentInstanceDataCache::ApplyToActor(AActor& Actor, ECacheApplyPhase Phase) const -> FCommandChangeResult
	{
		if (SourceActor.Resolve() != &Actor) return std::unexpected(FCommandChangeError{"Component instance cache owner mismatch."});
		if (auto Result = Validate(); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		std::unordered_set<const FActorComponentInstanceData*> Applied;
		for (const auto& Component : Actor.GetComponentsSnapshot())
			if (Component && IsValid(Component.Get()))
			{
				FActorGeneratedComponentKey Key;
				const auto* MatchKey = Actor.GetGeneratedComponentKey(Component.Get(), Key) ? &Key : nullptr;
				if (auto Valid = ValidateComponent(*Component, MatchKey); !Valid) return std::unexpected(FCommandChangeError{Valid.error()});
				for (const auto& Data : ComponentsInstanceData)
					if (Data && !Applied.contains(Data.get()) && Data->MatchesComponent(*Component))
					{
						if (auto Result = Data->ApplyToComponent(*Component, Phase); !Result) return Result;
						Applied.insert(Data.get()); break;
					}
			}
		return {};
	}
	auto FComponentInstanceDataCache::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		SourceActor.AddReferencedObjects(Collector);
		for (const auto& Data : ComponentsInstanceData) if (Data) Data->AddReferencedObjects(Collector);
	}
	auto FComponentInstanceDataCache::Serialize(FArchive& Archive) -> void
	{
		SerializeReference(Archive, SourceActor);
		uint64 Count = ComponentsInstanceData.size(); Archive << Count;
		if (Count > 1000000) { Archive.SetError("Component instance cache count exceeds the limit."); return; }
		if (Archive.IsLoading() && !Archive.IsError()) ComponentsInstanceData.resize(static_cast<size_t>(Count));
		for (auto& Data : ComponentsInstanceData)
		{
			if (Archive.IsError()) break;
			std::string TypeName;
			if (Archive.IsSaving())
			{
				if (!Data || Data->GetDataType().Type != typeid(*Data))
				{ Archive.SetError("Component instance data has no matching registered type."); return; }
				TypeName = Data->GetDataType().Name;
			}
			Archive << TypeName;
			if (Archive.IsError()) break;
			if (Archive.IsLoading())
			{
				const auto* Type = FActorComponentInstanceData::FindDataType(TypeName);
				if (!Type) { Archive.SetError("Unknown component instance data type."); return; }
				Data = Type->Construct();
			}

			if (!Archive.IsError()) Data->Serialize(Archive);
		}
	}
	auto FComponentInstanceDataCache::GetAllocatedSize() const -> size_t
	{
		constexpr auto Limit = std::numeric_limits<size_t>::max();
		size_t Total = sizeof(*this);
		auto Add = [&](size_t Size) { if (Size > Limit - Total) return false; Total += Size; return true; };
		if (ComponentsInstanceData.capacity() > Limit / sizeof(std::shared_ptr<FActorComponentInstanceData>)
			|| !Add(ComponentsInstanceData.capacity() * sizeof(std::shared_ptr<FActorComponentInstanceData>))) return Limit;
		for (const auto& Data : ComponentsInstanceData) if (Data && !Add(Data->GetAllocatedSize())) return Limit;
		return Total;
	}
}
