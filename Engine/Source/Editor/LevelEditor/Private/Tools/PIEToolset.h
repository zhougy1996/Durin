#pragma once

#include "CoreMinimal.h"
#include "Tools/NativeToolset.h"

namespace Durin
{
	// Module-owned tools resolve the admitting editor only inside protected calls.
	auto CreatePIEToolset() -> std::unique_ptr<FNativeToolset>;
}
