#pragma once

#ifndef DURIN_BUILD_DEBUG
	#define DURIN_BUILD_DEBUG 0
#endif

#ifndef DURIN_BUILD_SHIPPING
	#define DURIN_BUILD_SHIPPING 0
#endif

#if DURIN_BUILD_DEBUG
	#ifndef DO_CHECK
		#define DO_CHECK 1
	#endif
#endif

#if DURIN_BUILD_DEBUG
	#define DURIN_BUILD_TYPE_STRING "Debug"
#else
	#define DURIN_BUILD_TYPE_STRING "Release"
#endif

#if DURIN_BUILD_DEBUG
	#define DURIN_VISUALIZERS_HELPERS
#endif
