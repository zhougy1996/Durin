#pragma once

#include "LaunchAPI.h"

namespace Durin
{
	// Runs one fully owned application process and returns its public exit status.
	LAUNCH_API auto RunApplicationProcess(int ArgumentCount, char** Arguments) -> int;
}
