#include "ScopedSuspendRerunConstructionScripts.h"
#include <exception>

#include "Engine/Actor.h"
#include "DObject/ObjectLifecycle.h"
#include "Logging/LogMacros.h"

namespace Durin
{
	FScopedSuspendRerunConstructionScripts::FScopedSuspendRerunConstructionScripts(AActor& InActor, bool bInRerunOnExit)
		: Actor(&InActor), bRerunOnExit(bInRerunOnExit), ExceptionDepth(std::uncaught_exceptions())
	{ ++InActor.ConstructionSuspensionDepth; }
	FScopedSuspendRerunConstructionScripts::~FScopedSuspendRerunConstructionScripts()
	{
		FGarbageCollectionDeferralScope DeferCollection;
		auto* Value = Actor.Get();
		if (!Value) return;
		check(Value->ConstructionSuspensionDepth > 0);
		if (--Value->ConstructionSuspensionDepth != 0) return;
		const bool bRequested = std::exchange(Value->bNativeConstructionRequested, false);
		if (bRerunOnExit && bRequested && std::uncaught_exceptions() <= ExceptionDepth && !Value->IsBeingDestroyed())
			if (!Value->RerunConstructionScripts()) DURIN_ERROR("Deferred construction failed: {}", Value->GetConstructionError());
	}
}
