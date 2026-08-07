#pragma once

#ifndef DURIN_BUILD_DEBUG
	#define DURIN_BUILD_DEBUG 0
#endif

#ifndef DURIN_BUILD_RELEASE
	#define DURIN_BUILD_RELEASE 0
#endif

#ifndef DURIN_BUILD_SHIPPING
	#define DURIN_BUILD_SHIPPING 0
#endif

#if (DURIN_BUILD_DEBUG + DURIN_BUILD_RELEASE + DURIN_BUILD_SHIPPING) != 1
	#error Exactly one Durin build configuration must be active.
#endif

#ifdef DO_CHECK
	#undef DO_CHECK
#endif

#if DURIN_BUILD_DEBUG || DURIN_BUILD_RELEASE
	#define DO_CHECK 1
#else
	#define DO_CHECK 0
#endif

#if DURIN_BUILD_DEBUG
	#define DURIN_BUILD_TYPE_STRING "Debug"
#elif DURIN_BUILD_RELEASE
	#define DURIN_BUILD_TYPE_STRING "Release"
#else
	#define DURIN_BUILD_TYPE_STRING "Shipping"
#endif

#if DURIN_BUILD_DEBUG
	#define DURIN_VISUALIZERS_HELPERS
#endif
