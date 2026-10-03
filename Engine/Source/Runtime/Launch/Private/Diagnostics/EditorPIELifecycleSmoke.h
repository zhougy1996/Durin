#pragma once

#include "CoreMinimal.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	auto TryRunEditorPIELifecycleSmoke() -> bool;
#endif
}
