#pragma once

#include "CoreAPI.h"
#include "Misc/DurinVersion.gen.h"

namespace Durin
{
	// Represents the engine's semantic version and optional prerelease channel.
	struct FEngineVersion
	{
		uint32 Major = 0;
		uint32 Minor = 0;
		uint32 Patch = 0;
		std::string_view Channel;

		auto IsPrerelease() const -> bool { return !Channel.empty(); }
		auto operator==(const FEngineVersion&) const -> bool = default;
	};

	CORE_API auto GetEngineVersion() -> const FEngineVersion&;
	CORE_API auto GetEngineVersionString() -> std::string_view;
}
