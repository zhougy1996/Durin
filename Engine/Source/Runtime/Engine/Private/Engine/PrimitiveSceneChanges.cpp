#include "Engine/PrimitiveSceneChanges.h"

#if DURIN_WITH_EDITOR
#include "Components/PrimitiveComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"

namespace Durin
{
	namespace
	{
		auto CaptureMutation(DPrimitiveComponent* Component, bool bRetired) -> FPrimitiveSceneMutation
		{
			FPrimitiveSceneMutation Mutation;
			Mutation.Actor = Component->GetOwner();
			Mutation.Component = Component;
			Mutation.PrimitiveId = Component->GetPrimitiveComponentId();
			Mutation.RegistrationGeneration = Component->GetRegistrationGeneration();
			Mutation.bRetired = bRetired;
			return Mutation;
		}
	}

	auto FPrimitiveSceneChanges::CaptureSnapshot() const -> FPrimitiveSceneMutationBatch
	{
		FPrimitiveSceneMutationBatch Batch;
		Batch.Revision = Revision;
		Batch.bCompleteSnapshot = true;
		for (const TObjectPtr<AActor>& ActorPtr : Level.GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor) continue;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetComponents())
			{
				auto* Primitive = Cast<DPrimitiveComponent>(ComponentPtr.Get());
				if (Primitive && Primitive->IsRegistered())
					Batch.Mutations.push_back(CaptureMutation(Primitive, false));
			}
		}
		return Batch;
	}

	auto FPrimitiveSceneChanges::Subscribe(FObserver Observer) -> uint64
	{
		if (!Observer || bDispatching) return 0;
		const uint64 Subscription = NextObserverId++;
		Observers.emplace(Subscription, Observer);
		bDispatching = true;
		Observer(CaptureSnapshot());
		bDispatching = false;
		return Subscription;
	}

	auto FPrimitiveSceneChanges::Unsubscribe(uint64 Subscription) -> void
	{
		Observers.erase(Subscription);
	}

	auto FPrimitiveSceneChanges::Notify(DPrimitiveComponent* Component, bool bRetired) -> void
	{
		if (!Component) return;
		require(!bDispatching);
		++Revision;
		if (Observers.empty()) return;
		FPrimitiveSceneMutationBatch Batch;
		Batch.Revision = Revision;
		Batch.Mutations.push_back(CaptureMutation(Component, bRetired));
		std::vector<FObserver> PendingObservers;
		PendingObservers.reserve(Observers.size());
		for (const auto& [Id, Observer] : Observers)
		{
			(void)Id;
			PendingObservers.push_back(Observer);
		}
		bDispatching = true;
		for (const FObserver& Observer : PendingObservers) Observer(Batch);
		bDispatching = false;
	}
}
#endif
