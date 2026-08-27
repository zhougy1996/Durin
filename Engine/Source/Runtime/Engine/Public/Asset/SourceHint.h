#pragma once

#include "EngineAPI.h"

namespace Durin
{
	enum class ESourceHintBase : uint8;
	inline constexpr size_t MaximumSourceHintBytes = 4'096;

	// Classifies a captured source against an explicit or canonical default base.
	ENGINE_API auto MakeSourceHint(
		std::string_view PhysicalPath,
		std::string_view OwningPackagePhysicalPath,
		ESourceHintBase& OutBase,
		std::string& OutHint,
		std::string& OutError,
		std::optional<ESourceHintBase> RequestedBase = {}) -> bool;
	// Resolves an optional hint for an explicit reimport action only.
	ENGINE_API auto ResolveSourceHint(
		ESourceHintBase Base,
		std::string_view Hint,
		std::string_view OwningPackagePhysicalPath,
		std::string& OutPhysicalPath,
		std::string& OutError) -> bool;
}
