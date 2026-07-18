#pragma once

#include "CoreAPI.h"
#include "Misc/DurinVersion.gen.h"

namespace Durin
{
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
