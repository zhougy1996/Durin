#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class FRHIResource;

	// Experimental allocation policy. Sharing still requires an explicit ordered handoff.
	enum class ERHITransientAliasing : uint8 { Disabled, Enabled };

	// Null Before acquires pooled backing whose previous graph uses are already ordered.
	// Recording retains both resources; this barrier does not itself order different queues.
	struct FRHITransientAliasingBarrier final
	{
		FRHIResource* Before = nullptr;
		FRHIResource* After = nullptr;
	};
}
