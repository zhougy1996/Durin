#pragma once

#include "CoreMinimal.h"

namespace Durin::MonaImGui
{
	// Backend shutdown withdraws all callbacks before native windows retire.
	auto ResetViewportClosePolicies() -> void;
}
