#pragma once

#include "Definitions.Core.h"
#include "Definitions.RHI.h"

#ifdef VULKAN_RHI_EXPORTS
	#define VULKAN_RHI_API DLLEXPORT
#else
	#define VULKAN_RHI_API DLLIMPORT
#endif