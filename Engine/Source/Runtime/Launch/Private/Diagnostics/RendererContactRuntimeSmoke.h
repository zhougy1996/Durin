#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	struct FRendererContactRuntimeSmokeState;

	// Creates the retained state used to qualify contact routes across real frames.
	auto BeginRendererContactRuntimeSmoke()
		-> std::shared_ptr<FRendererContactRuntimeSmokeState>;

	// Advances one pre-render state-machine step and reports terminal success.
	auto TickRendererContactRuntimeSmoke(
		const std::shared_ptr<FRendererContactRuntimeSmokeState>& State) -> bool;

	// Restores view policy, window geometry, and auxiliary-view ownership.
	auto EndRendererContactRuntimeSmoke(
		const std::shared_ptr<FRendererContactRuntimeSmokeState>& State) -> void;
}
