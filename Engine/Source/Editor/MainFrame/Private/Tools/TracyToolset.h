#pragma once

#include "CoreMinimal.h"
#include "Tools/NativeToolset.h"

namespace Durin::MainFrame
{
	class FProfilingToolService;
	// Adapts managed Tracy tools without owning any interactive UI state.
	auto CreateTracyToolset(std::shared_ptr<FProfilingToolService> Service) -> std::unique_ptr<FNativeToolset>;
}
