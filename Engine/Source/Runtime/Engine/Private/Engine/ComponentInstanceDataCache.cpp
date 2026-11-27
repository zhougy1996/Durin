#include "Engine/ComponentInstanceDataCache.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
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
	FActorRootComponentReconstructionData::FActorRootComponentReconstructionData(const AActor& Actor, const FActorRootComponentReconstructionData* Previous)
		: SourceActor(const_cast<AActor*>(&Actor))
	{
		auto* RootComponent = Actor.GetRootComponent();
		Root = FActorComponentInstanceSourceInfo(RootComponent);
		bCachedGeneratedRoot = RootComponent && RootComponent->GetCreationMethod() == EComponentCreationMethod::Generated;
		if (bCachedGeneratedRoot)
		{
			RootParent = FActorComponentInstanceSourceInfo(RootComponent->GetAttachParent());
			RootRelative = RootComponent->GetRelativeTransform();
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
	auto FActorRootComponentReconstructionData::ApplyToActor(AActor& Actor, bool bRequireRoot) const -> std::expected<void, std::string>
	{
		if (auto Result = Validate(); !Result) return Result;
		if (SourceActor.Resolve() != &Actor) return std::unexpected("Root reconstruction owner mismatch.");
		auto* SceneRoot = Cast<DSceneComponent>(Root.Resolve());
		auto* Parent = Cast<DSceneComponent>(RootParent.Resolve());
		const bool bRestoreRoot = bRequireRoot || (SceneRoot && Actor.GetRootComponent() == SceneRoot);
		if (bRestoreRoot && ((!Root.IsNull() && !SceneRoot) || (SceneRoot && SceneRoot->GetOwner() != &Actor)))
			return std::unexpected("Actor transaction root identity is unavailable.");
		if (bRestoreRoot && bCachedGeneratedRoot)
		{
			if (!SceneRoot || SceneRoot->GetOwner() != &Actor || (!RootParent.IsNull() && !Parent)
				|| (Parent && (Parent == SceneRoot || Parent->IsAttachedTo(SceneRoot))))
				return std::unexpected("Actor generated-root attachment snapshot cannot resolve safely.");
		}
		for (const auto& Child : Attached)
		{
			auto* Component = Cast<DSceneComponent>(Child.Target.Resolve());
			auto* ParentComponent = Cast<DSceneComponent>(Child.Parent.Resolve());
			if (!bRequireRoot && !Child.Parent.IsNull() && !ParentComponent) continue;
			if (!Component || (!Child.Parent.IsNull() && !ParentComponent)
				|| (ParentComponent && (ParentComponent == Component || ParentComponent->IsAttachedTo(Component))))
				return std::unexpected("Actor attachment recovery participant cannot resolve safely.");
		}
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bRequireRoot && !Actor.SetRootComponent(SceneRoot)) return std::unexpected("Actor transaction root could not be reconciled.");
		if (bRestoreRoot && bCachedGeneratedRoot)
		{
			const bool AttachedRoot = SceneRoot->GetAttachParent() == Parent || (Parent ? SceneRoot->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative)
				: SceneRoot->DetachFromComponent(EDetachmentTransformRule::KeepRelative));
			if (!AttachedRoot) return std::unexpected("Actor generated-root attachment restore failed.");
			if (bRequireRoot || !SceneRoot->IsTransformModifiedByConstructionScript()) SceneRoot->SetRelativeTransform(RootRelative);
		}
		for (const auto& Child : Attached)
		{
			auto* Component = Cast<DSceneComponent>(Child.Target.Resolve());
			auto* ParentComponent = Cast<DSceneComponent>(Child.Parent.Resolve());
			if (!bRequireRoot && !Child.Parent.IsNull() && !ParentComponent) continue;
			if (!Component) return std::unexpected("Actor attachment recovery target expired.");
			const bool Restored = Component->GetAttachParent() == ParentComponent || (ParentComponent
				? Component->AttachToComponent(ParentComponent, EAttachmentTransformRule::KeepRelative)
				: Component->DetachFromComponent(EDetachmentTransformRule::KeepRelative));
			if (!Restored) return std::unexpected("Actor attachment recovery failed.");
			Component->SetRelativeTransform(Child.Relative);
		}
		return {};
	}
	auto FActorRootComponentReconstructionData::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		SourceActor.AddReferencedObjects(Collector); Root.AddReferencedObjects(Collector); RootParent.AddReferencedObjects(Collector);
		for (const auto& Child : Attached) { Child.Target.AddReferencedObjects(Collector); Child.Parent.AddReferencedObjects(Collector); }
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

	auto FActorComponentInstanceData::GetDataType() const -> const FComponentInstanceDataType&
	{ return CopyType<FActorComponentInstanceData>(); }
	auto FSceneComponentInstanceData::GetDataType() const -> const FComponentInstanceDataType&
	{ return CopyType<FSceneComponentInstanceData>(); }
	FActorComponentInstanceData::FActorComponentInstanceData(const DActorComponent& Component) : Source(&Component) {}
	auto FActorComponentInstanceData::MatchesComponent(const DActorComponent& Component, const FActorGeneratedComponentKey* Key) const -> bool
	{ return Source.MatchesComponent(Component, Key); }
	auto FActorComponentInstanceData::ApplyToComponent(DActorComponent&) const -> void {}
	auto FActorComponentInstanceData::AddReferencedObjects(FReferenceCollector& Collector) const -> void { Source.AddReferencedObjects(Collector); }

	FSceneComponentInstanceData::FSceneComponentInstanceData(const DActorComponent& Component)
		: FActorComponentInstanceData(Component), Relative(static_cast<const DSceneComponent&>(Component).GetRelativeTransform()) {}
	auto FSceneComponentInstanceData::ApplyToComponent(DActorComponent& Component) const -> void
	{
		auto* Scene = Cast<DSceneComponent>(&Component);
		if (!Scene) throw std::runtime_error("Scene instance data requires the same scene component class.");
		if (!Scene->IsTransformModifiedByConstructionScript()) Scene->SetRelativeTransform(Relative);
	}

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

	auto FComponentInstanceDataCache::ApplyToActor(AActor& Actor) const -> void
	{
		std::unordered_set<const FActorComponentInstanceData*> Applied;
		for (const auto& Component : Actor.GetComponentsSnapshot())
			if (Component && IsValid(Component.Get()))
			{
				FActorGeneratedComponentKey Key;
				const auto* MatchKey = Actor.GetGeneratedComponentKey(Component.Get(), Key) ? &Key : nullptr;
				if (auto Valid = ValidateComponent(*Component, MatchKey); !Valid) throw std::runtime_error(Valid.error());
				for (const auto& Data : ComponentsInstanceData)
					if (Data && !Applied.contains(Data.get()) && Data->MatchesComponent(*Component))
					{
						Data->ApplyToComponent(*Component);
						Applied.insert(Data.get()); break;
					}
			}
	}
	auto FComponentInstanceDataCache::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		SourceActor.AddReferencedObjects(Collector);
		for (const auto& Data : ComponentsInstanceData) if (Data) Data->AddReferencedObjects(Collector);
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
