#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"

#include "SourcePath.gen.h"

namespace Durin
{
	// Stores one portable file path in a mount's SourceAssets domain.
	DSTRUCT()
	struct FSourcePath
	{
		GENERATED_BODY()

		// Empty means no source dependency; otherwise this is a complete normalized virtual file path.
		DPROPERTY()
		std::string Path;

		auto IsEmpty() const -> bool { return Path.empty(); }
		auto operator==(const FSourcePath&) const -> bool = default;
	};
}
