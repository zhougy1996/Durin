#pragma once

#include "CoreAPI.h"

namespace Durin
{
	// Moves one legacy file only when its destination is absent, with copy/remove fallback.
	CORE_API auto MigrateLegacyFileIfMissing(
		const std::filesystem::path& LegacyPath,
		const std::filesystem::path& DestinationPath,
		std::string* OutWarning = nullptr) -> bool;
}
