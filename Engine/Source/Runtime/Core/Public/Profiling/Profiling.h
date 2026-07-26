#pragma once

#ifndef DURIN_WITH_TRACY
	#define DURIN_WITH_TRACY 0
#endif

#if DURIN_WITH_TRACY
	#include <tracy/Tracy.hpp>

	#define DURIN_PROFILE_CPU_ZONE() ZoneScoped
	#define DURIN_PROFILE_CPU_ZONE_NAMED(Name) ZoneScopedN(Name)
	#define DURIN_PROFILE_FRAME_MARK() FrameMark
	#define DURIN_PROFILE_THREAD(Name) tracy::SetThreadName(Name)
#else
	#define DURIN_PROFILE_CPU_ZONE() ((void)0)
	#define DURIN_PROFILE_CPU_ZONE_NAMED(Name) ((void)0)
	#define DURIN_PROFILE_FRAME_MARK() ((void)0)
	#define DURIN_PROFILE_THREAD(Name) ((void)0)
#endif
