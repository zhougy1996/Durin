#pragma once

#include "MonaCoreAPI.h"
#include "MonaCoreFwd.h"

namespace Durin::Mona
{
	// Installs one process-wide backend without transferring ownership. Host
	// composition performs registration on its module-control thread.
	MONACORE_API auto RegisterUIBackend(IMonaUIBackend& Backend) -> bool;

	// Removes only the currently installed instance, rejecting mismatched teardown.
	MONACORE_API auto UnregisterUIBackend(IMonaUIBackend& Backend) -> bool;

	MONACORE_API auto GetActiveUIBackend() -> IMonaUIBackend*;
}
