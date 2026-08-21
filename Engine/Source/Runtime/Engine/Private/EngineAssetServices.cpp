#include "EngineAssetServices.h"

namespace Durin
{
	auto InitializeEngineAssetServices() -> void
	{
		// Engine asset types currently own no deletion companions. Mounted sources
		// are shared inputs and remain under their explicit source operations.
	}
}
