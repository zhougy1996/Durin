#pragma once

#include "CoreMinimal.h"
#include "Tools/NativeToolset.h"

namespace Durin::MainFrame
{
	// Exposes on-demand process-wide statistics without retaining sampled state.
	auto CreateNamePoolToolset() -> std::unique_ptr<FNativeToolset>;
}
