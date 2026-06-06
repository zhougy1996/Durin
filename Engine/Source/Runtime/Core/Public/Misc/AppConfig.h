#pragma once

#include "CoreAPI.h"
#include "CoreFwd.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	extern CORE_API FYamlNodeView GAppConfig;

	CORE_API auto LoadAppConfig(std::string_view ConfigFile) -> bool;
}
