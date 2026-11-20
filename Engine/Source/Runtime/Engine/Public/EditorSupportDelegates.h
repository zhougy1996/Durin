#pragma once

#include "CoreMinimal.h"

#if DURIN_WITH_EDITOR
#include "Delegates/Delegate.h"
#include "EngineAPI.h"

namespace Durin
{
	class DLevel;

	struct FEditorSupportDelegates
	{
		// Game-thread request for viewports displaying this level to discard cached and pending picks.
		static ENGINE_API TMulticastDelegate<void(DLevel&)> InvalidateHitProxies;
		static ENGINE_API TMulticastDelegate<void(DLevel&)> ActorHierarchyChanged;
	};
}
#endif
