#pragma once

#include "Misc/CoreStd.h"

#include "CoreAPI.h"
#include "CoreFwd.h"
#include "Yaml/Yaml.h"

namespace Durin
{
    // Maps merge recursively; scalars and sequences replace. Missing optional files are ignored.
    // Output is unchanged on malformed YAML or I/O failure; no source file is written.
    CORE_API auto LoadConfigLayers(std::span<const std::filesystem::path> Files,
        FYamlDocument& OutDocument, std::string& OutReason) -> bool;
    // Startup-only publication, before module consumers retain configuration views.
    CORE_API auto LoadAppConfigLayers(std::span<const std::filesystem::path> Files, std::string& OutReason) -> bool;
	CORE_API auto LoadAppConfig(std::string_view ConfigFile) -> bool;
	// Modules receive only their top-level section; a missing section produces an invalid view.
	CORE_API auto GetModuleConfig(std::string_view ModuleName) -> FYamlNodeView;
}
