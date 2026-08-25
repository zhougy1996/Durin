#pragma once

#include "AssetCoreAPI.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"

#include "SourcePath.gen.h"

namespace Durin
{
	// Stores one portable authoring-file path in a registered mount.
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
