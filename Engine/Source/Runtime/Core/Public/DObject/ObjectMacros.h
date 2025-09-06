#pragma once

#define DCLASS(...)
#define DPROPERTY(...)
#define DFUNCTION(...)

#define BODY_MACRO_COMBINE_INNER(A,B,C,D) A##B##C##D
#define BODY_MACRO_COMBINE(A,B,C,D) BODY_MACRO_COMBINE_INNER(A,B,C,D)

#ifdef __INTELLISENSE__
	#define GENERATED_BODY(...)
#else
	#define GENERATED_BODY(...) BODY_MACRO_COMBINE(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)
#endif
