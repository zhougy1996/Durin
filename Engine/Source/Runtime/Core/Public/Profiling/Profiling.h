#pragma once

#include "CoreAPI.h"

#include <string>
#include <string_view>

#ifndef DURIN_WITH_TRACY
	#define DURIN_WITH_TRACY 0
#endif

namespace Durin::Profiling
{
	CORE_API auto FormatProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string;
	CORE_API auto SetProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string_view;
	CORE_API auto GetProgramIdentity() -> std::string;
}

#if DURIN_WITH_TRACY
	#include <tracy/Tracy.hpp>

	#define DURIN_PROFILE_CPU_ZONE() ZoneScoped
	#define DURIN_PROFILE_CPU_ZONE_NAMED(Name) ZoneScopedN(Name)
	#define DURIN_PROFILE_FRAME_MARK() FrameMark
	#define DURIN_PROFILE_THREAD(Name) tracy::SetThreadName(Name)
	#define DURIN_PROFILE_PROGRAM_IDENTITY(RuntimeVariant, ProjectName, ProcessId) \
		::Durin::Profiling::SetProgramIdentity(RuntimeVariant, ProjectName, ProcessId)
#else
	#define DURIN_PROFILE_CPU_ZONE() ((void)0)
	#define DURIN_PROFILE_CPU_ZONE_NAMED(Name) ((void)0)
	#define DURIN_PROFILE_FRAME_MARK() ((void)0)
	#define DURIN_PROFILE_THREAD(Name) ((void)0)
	#define DURIN_PROFILE_PROGRAM_IDENTITY(RuntimeVariant, ProjectName, ProcessId) ((void)0)
#endif
