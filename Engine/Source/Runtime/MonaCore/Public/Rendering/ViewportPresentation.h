#pragma once

#include "CoreMinimal.h"
#include "MonaCoreAPI.h"
#include "Rendering/ViewportDisplaySource.h"

namespace Durin
{
	class FRHICommandListBase;
	using FConsumedViewportOutputs = std::vector<std::pair<std::shared_ptr<FViewportOutputReceipt>, uint64>>;
	// Observe the presentation during command replay, never immediately after
	// recording. Both observations and the native Present are ordered in one stream.
	MONACORE_API auto EndDrawingViewportWithReceipts(FRHICommandListBase& Commands,
		const FViewportRHIRef& Viewport, bool bPresent, FConsumedViewportOutputs Outputs) -> void;
}
