#pragma once

#include "CoreMinimal.h"

namespace Durin::MaterialBuildPrivate
{
	// Bump for changes to normalization/folding or generated Slang, respectively.
	inline constexpr uint32 NormalizationAlgorithmRevision = 8;
	inline constexpr uint32 GeneratorAlgorithmRevision = 14;
	// Request/result adaptation is independent of algorithms and cooked payloads.
	inline constexpr uint32 CompileAdapterVersion = 12;
}
