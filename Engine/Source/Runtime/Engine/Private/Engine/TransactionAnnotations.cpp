#include "Engine/TransactionAnnotations.h"

#include "Components/ActorComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectGraphReplacement.h"
#include "Engine/Actor.h"

namespace Durin
{

	auto FActorTransactionAnnotation::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		FActorTransactionAnnotation Rebound(*this);
		auto Key = Map.RemapObjectKey(Rebound.Data.Target.GetKey());
		if (!Key) return std::unexpected(ToString(Key.error()));
		Rebound.Data.Target = FPersistentObjectRef::FromKey(*Key);
		if (auto Valid = Rebound.Data.ComponentInstanceData.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		if (auto Valid = Rebound.Data.RootData.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		if (auto Valid = Rebound.Validate(); !Valid) return Valid;
		Data = std::move(Rebound.Data);
		return {};
	}

	FActorTransactionAnnotation::FActorTransactionAnnotation(const AActor& Actor, const FActorTransactionAnnotation* Previous)
		: Data{FPersistentObjectRef(const_cast<AActor*>(&Actor)),
			FComponentInstanceDataCache(Actor), FActorRootComponentReconstructionData(Actor, Previous ? &Previous->Data.RootData : nullptr)}
	{}

	auto FActorTransactionAnnotation::CreateIfRequired(const AActor& Actor) -> std::shared_ptr<FActorTransactionAnnotation>
	{
		auto Annotation = std::make_shared<FActorTransactionAnnotation>(Actor);
		// Generated components need a historical endpoint even when all use their defaults.
		if (Annotation->Data.ComponentInstanceData.HasInstanceData() || Annotation->Data.RootData.HasInstanceData()
			|| std::ranges::any_of(Actor.GetComponents(), [](const auto& Component) {
				return Component && Component->GetCreationMethod() == EComponentCreationMethod::Generated;
			})) return Annotation;
		return nullptr;
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
		if (auto Result = Data.ComponentInstanceData.Validate(); !Result) return Result;
		return Data.RootData.Validate();
	}
	auto FActorTransactionAnnotation::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Data.Target.AddReferencedObjects(Collector);
		Data.ComponentInstanceData.AddReferencedObjects(Collector);
		Data.RootData.AddReferencedObjects(Collector);
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
		return sizeof(*this) + CacheBytes + RootBytes;
	}
}
