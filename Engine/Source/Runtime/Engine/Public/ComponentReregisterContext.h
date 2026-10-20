#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Components/ActorComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	// Temporarily removes registration/resources, retaining initialization and Play.
	class FComponentReregisterContext
	{
	public:
		ENGINE_API explicit FComponentReregisterContext(DActorComponent* Component);
		ENGINE_API ~FComponentReregisterContext();
		FComponentReregisterContext(const FComponentReregisterContext&) = delete;
		auto operator=(const FComponentReregisterContext&) -> FComponentReregisterContext& = delete;
		ENGINE_API auto ReRegister() -> std::expected<void, std::string>;
	private:
		FGarbageCollectionDeferralScope DeferCollection;
		TWeakObjectPtr<DActorComponent> Component;
		TWeakObjectPtr<AActor> Owner;
		bool bTickRegistered = false;
		bool bInitialized = false;
		bool bPending = false;
	};
}
