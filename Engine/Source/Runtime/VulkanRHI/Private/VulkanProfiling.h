#pragma once

#include "Profiling/Profiling.h"

// Keep per-draw and binding detail out of ordinary captures. Submission and wait
// zones use the standard profiling macros independently of this option.
#if DURIN_VULKAN_DETAILED_PROFILING
	#define DURIN_VULKAN_PROFILE_DETAIL(Name) DURIN_PROFILE_CPU_ZONE_NAMED(Name)
#else
	#define DURIN_VULKAN_PROFILE_DETAIL(Name) ((void)0)
#endif
