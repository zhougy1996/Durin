#pragma once
#include "Materials/MaterialInterface.h"

namespace Durin::Private
{
	// Engine-internal aggregation; the public API exposes copies only.
	auto GetMutableMaterialLoadedQueryDiagnostics() -> FMaterialLoadedQueryDiagnostics&;
}
