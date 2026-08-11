#pragma once

#if defined(LAUNCH_EXPORTS)
	#if defined(_WIN32)
		#define LAUNCH_API __declspec(dllexport)
	#else
		#define LAUNCH_API __attribute__((visibility("default")))
	#endif
#else
	#if defined(_WIN32)
		#define LAUNCH_API __declspec(dllimport)
	#else
		#define LAUNCH_API __attribute__((visibility("default")))
	#endif
#endif
