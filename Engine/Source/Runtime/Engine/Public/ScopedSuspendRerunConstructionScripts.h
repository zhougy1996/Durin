#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class AActor;
	// Durin's per-Actor scope; UE's equivalent suspension is global.
	class FScopedSuspendRerunConstructionScripts
	{
	public:
		ENGINE_API explicit FScopedSuspendRerunConstructionScripts(AActor& Actor, bool bRerunOnExit = true);
		ENGINE_API ~FScopedSuspendRerunConstructionScripts();
		FScopedSuspendRerunConstructionScripts(const FScopedSuspendRerunConstructionScripts&) = delete;
		auto operator=(const FScopedSuspendRerunConstructionScripts&) -> FScopedSuspendRerunConstructionScripts& = delete;
	private:
		TWeakObjectPtr<AActor> Actor;
		bool bRerunOnExit;
		int32 ExceptionDepth = 0;
	};
}
