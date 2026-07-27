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

	// Converts one pre-mounted SourceAssets-relative carrier using the owning
	// package's Content mount and requires the referenced source file to exist.
	ENGINE_API auto TryMigrateLegacySourcePath(
		std::string_view PackagePath,
		std::string_view LegacyPath,
		FSourcePath& OutSourcePath,
		std::filesystem::path& OutPhysicalPath,
		std::string& OutError) -> bool;
}
