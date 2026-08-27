#pragma once

#include "EngineAPI.h"

namespace Durin::AssetImport
{
	inline constexpr size_t MaximumSourceFilenameBytes = 4'096;

	// Converts a physical source path to the persisted project-relative or absolute form.
	ENGINE_API auto MakeSourceFilename(
		std::string_view PhysicalPath,
		std::string& OutFilename,
		std::string& OutError) -> bool;
	// Resolves a persisted filename without consulting package mounts or mutating the source.
	ENGINE_API auto ResolveSourceFilename(
		std::string_view Filename,
		std::string& OutPhysicalPath,
		std::string& OutError) -> bool;
}
