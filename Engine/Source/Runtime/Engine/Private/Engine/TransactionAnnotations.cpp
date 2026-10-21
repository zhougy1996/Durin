#include "Engine/TransactionAnnotations.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectGraphReplacement.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	auto FComponentInstanceLifecycleState::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		auto Result = *this;
		if (auto Valid = Result.Target.FindAndReplaceInstances(Map); !Valid) return Valid;
		if (auto Valid = Result.ValidateReferences(); !Valid) return Valid;
		*this = std::move(Result);
		return {};
	}

	auto FComponentTransactionAnnotation::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>
	{
		auto Result = std::make_shared<FComponentTransactionAnnotation>(*this);
		if (auto Valid = Result->Data.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto FActorTransactionAnnotation::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>
	{
		auto Result = std::make_shared<FActorTransactionAnnotation>(*this);
		auto Key = Map.RemapObjectKey(Result->Data.Target.GetKey());
		if (!Key) return std::unexpected(ToString(Key.error()));
		Result->Data.Target = FPersistentObjectRef::FromKey(*Key);
		if (auto Valid = Result->Data.ComponentInstanceData.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		if (auto Valid = Result->Data.RootData.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		for (auto& State : Result->Data.ComponentLifecycle)
			if (auto Valid = State.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		if (auto Valid = Result->Validate(); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	namespace
	{
		auto SerializeReference(FArchive& Archive, FPersistentObjectRef& Reference) -> void
		{
			auto Key = Reference.GetKey(); Key.SerializeForSnapshot(Archive);
			if (Archive.IsLoading() && !Archive.IsError()) Reference = FPersistentObjectRef::FromKey(Key);
		}
		auto GetDefaultActorTickState(const AActor& Actor) -> FTickFunctionState
		{
			FTickFunctionState State;
			auto* Level = Cast<DLevel>(Actor.GetOuter());
			auto* World = Level ? Level->GetWorld() : nullptr;
			State.bRegistered = World && World->GetCurrentLevel() == Level && Level->ContainsActor(&Actor);
			return State;
		}

	}

	FComponentInstanceLifecycleState::FComponentInstanceLifecycleState(const DActorComponent& Component)
		: Target(&Component), State(Component.CaptureLifecycleState()) {}
	auto FComponentInstanceLifecycleState::IsDefault() const -> bool { return State == FActorComponentLifecycleState{}; }
	auto FComponentInstanceLifecycleState::DefaultFor(const DActorComponent& Component) -> FComponentInstanceLifecycleState
	{
		auto Data = FComponentInstanceLifecycleState(Component);
		Data.State = {};
		return Data;
	}
	auto FComponentInstanceLifecycleState::Validate() const -> FTransactionCaptureResult
	{
		if (auto Result = ValidateReferences(); !Result) return Result;
		auto* Component = Target.Resolve();
		if (!Component) return std::unexpected("Component lifecycle participant is unavailable.");
		return Component->ValidateLifecycleState(State);
	}
	auto FComponentInstanceLifecycleState::ValidateReferences() const -> FTransactionCaptureResult
	{
		if (Target.IsNull()) return std::unexpected("Component lifecycle owner or target expired.");
		return Target.Validate();
	}
	auto FComponentInstanceLifecycleState::Restore() const -> FCommandChangeResult
	{
		if (auto Valid = Validate(); !Valid) return std::unexpected(FCommandChangeError{Valid.error()});
		if (auto Result = Target.Resolve()->ApplyLifecycleState(State); !Result)
			return std::unexpected(FCommandChangeError{Result.error(), true});
		return {};
	}
	auto FComponentInstanceLifecycleState::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{ Target.AddReferencedObjects(Collector); }
	auto FComponentInstanceLifecycleState::Serialize(FArchive& Ar) -> void
	{ Target.Serialize(Ar); DActorComponent::SerializeLifecycleState(Ar, State); }

	FActorTransactionAnnotation::FActorTransactionAnnotation(const AActor& Actor, const FActorTransactionAnnotation* Previous)
		: Data{FPersistentObjectRef(const_cast<AActor*>(&Actor)), Actor.GetPrimaryActorTick().CaptureState(),
			FComponentInstanceDataCache(Actor), {}, FActorRootComponentReconstructionData(Actor, Previous ? &Previous->Data.RootData : nullptr)}
	{
		for (const auto& Component : Actor.GetComponentsSnapshot())
			if (Component) Data.ComponentLifecycle.emplace_back(*Component);
	}

	auto FActorTransactionAnnotation::CreateIfRequired(const AActor& Actor) -> std::shared_ptr<FActorTransactionAnnotation>
	{
		auto Annotation = std::make_shared<FActorTransactionAnnotation>(Actor);
		return Annotation->HasInstanceData() ? Annotation : nullptr;
	}
	auto FActorTransactionAnnotation::HasInstanceData() const -> bool
	{
		auto* Actor = Cast<AActor>(Data.Target.Resolve());
		if (!Actor) return true;
		const auto DefaultTick = GetDefaultActorTickState(*Actor);
		return Data.ComponentInstanceData.HasInstanceData() || Data.RootData.HasInstanceData()
			|| std::ranges::any_of(Data.ComponentLifecycle, [](const auto& State) { return !State.IsDefault(); })
			|| Data.Tick.Group != DefaultTick.Group || Data.Tick.bEnabled != DefaultTick.bEnabled
			|| Data.Tick.bRegistered != DefaultTick.bRegistered;
	}
	auto FActorTransactionAnnotation::RestoreDefault(AActor& Actor) -> FCommandChangeResult
	{
		if (auto Result = Actor.GetPrimaryActorTick().RestoreState(GetDefaultActorTickState(Actor), Cast<DLevel>(Actor.GetOuter())); !Result)
			return std::unexpected(FCommandChangeError{Result.error(), true});
		return {};
	}

	auto FActorTransactionAnnotation::CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		auto* Actor = Cast<AActor>(const_cast<DObject*>(&Object));
		if (!Actor || Data.Target.Resolve() != Actor) throw std::runtime_error("Actor annotation snapshot refresh target is incompatible.");
		return std::make_shared<FActorTransactionAnnotation>(*Actor, this);
	}
	auto FActorTransactionAnnotation::Validate() const -> FTransactionCaptureResult
	{
		auto* Actor = Cast<AActor>(Data.Target.Resolve());
		if (!Actor || Actor->IsBeingDestroyed()) return std::unexpected("Actor annotation target expired.");
		if (auto Result = FTickFunction::ValidateState(Data.Tick, Cast<DLevel>(Actor->GetOuter())); !Result) return Result;
		if (auto Result = Data.ComponentInstanceData.Validate(); !Result) return Result;
		for (const auto& State : Data.ComponentLifecycle) if (auto Result = State.ValidateReferences(); !Result) return Result;
		return Data.RootData.Validate();
	}
	auto FActorTransactionAnnotation::Restore(AActor& Actor, bool bApplyRootData) const -> FCommandChangeResult
	{
		if (auto Result = Validate(); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		if (Data.Target.Resolve() != &Actor) return std::unexpected(FCommandChangeError{"Actor annotation target mismatch."});
		if (auto Result = Actor.GetPrimaryActorTick().RestoreState(Data.Tick, Cast<DLevel>(Actor.GetOuter())); !Result)
			return std::unexpected(FCommandChangeError{Result.error(), true});
		if (bApplyRootData) if (auto Result = Data.RootData.ApplyToActor(Actor); !Result) return Result;
		for (const auto& State : Data.ComponentLifecycle) if (auto Result = State.Validate(); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		for (const auto& State : Data.ComponentLifecycle) if (auto Result = State.Restore(); !Result) return Result;
		return {};
	}
	auto FActorTransactionAnnotation::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Data.Target.AddReferencedObjects(Collector);
		Data.ComponentInstanceData.AddReferencedObjects(Collector);
		Data.RootData.AddReferencedObjects(Collector);
		for (const auto& State : Data.ComponentLifecycle) State.AddReferencedObjects(Collector);
	}
	auto FActorTransactionAnnotation::Serialize(FArchive& Archive) -> void
	{
		SerializeReference(Archive, Data.Target);
		FTickFunction::SerializeState(Archive, Data.Tick);
		Data.ComponentInstanceData.Serialize(Archive);
		Data.RootData.Serialize(Archive);
		uint64 Count = Data.ComponentLifecycle.size(); Archive << Count;
		if (Count > 1000000) { Archive.SetError("Component lifecycle annotation count exceeds the limit."); return; }
		if (Archive.IsLoading() && !Archive.IsError()) Data.ComponentLifecycle.resize(static_cast<size_t>(Count));
		for (auto& State : Data.ComponentLifecycle) { if (Archive.IsError()) break; State.Serialize(Archive); }
	}
	auto FActorTransactionAnnotation::GetAllocatedSize() const -> size_t
	{
		constexpr auto Limit = std::numeric_limits<size_t>::max();
		const auto CacheSize = Data.ComponentInstanceData.GetAllocatedSize(), RootSize = Data.RootData.GetAllocatedSize();
		if (CacheSize == Limit || RootSize == Limit) return Limit;
		// Cache/root objects are inline in Data; count only their dynamic storage again.
		const auto CacheBytes = CacheSize - sizeof(Data.ComponentInstanceData);
		const auto RootBytes = RootSize - sizeof(Data.RootData);
		if (CacheBytes > Limit - sizeof(*this) || RootBytes > Limit - sizeof(*this) - CacheBytes) return Limit;
		const auto BaseBytes = sizeof(*this) + CacheBytes + RootBytes;
		if (Data.ComponentLifecycle.capacity() > (Limit - BaseBytes) / sizeof(FComponentInstanceLifecycleState)) return Limit;
		return BaseBytes + Data.ComponentLifecycle.capacity() * sizeof(FComponentInstanceLifecycleState);
	}
}
