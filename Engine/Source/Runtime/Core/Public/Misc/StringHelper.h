#pragma once

#include "CoreAPI.h"

namespace Durin::StringUtils
{
	[[nodiscard]] CORE_API auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool;
	[[nodiscard]] CORE_API auto HumanizeName(std::string_view Name) -> std::string;
}
