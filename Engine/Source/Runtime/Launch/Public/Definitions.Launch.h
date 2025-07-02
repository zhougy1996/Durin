#pragma once

#ifdef LAUNCH_EXPORTS
	#define LAUNCH_API DLLEXPORT
#else
	#define LAUNCH_API DLLIMPORT
#endif