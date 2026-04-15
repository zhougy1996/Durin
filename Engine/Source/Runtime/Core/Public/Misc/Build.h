#pragma once

#ifndef DOGE_BUILD_DEBUG
	#define DOGE_BUILD_DEBUG 0
#endif

#if DOGE_BUILD_DEBUG
	#ifndef DO_CHECK
		#define DO_CHECK 1
	#endif
#endif

#if DOGE_BUILD_DEBUG
	#define DOGE_BUILD_TYPE_STRING "Debug"
#else
	#define DOGE_BUILD_TYPE_STRING "Release"
#endif

#define DOGE_VISUALIZERS_HELPERS
