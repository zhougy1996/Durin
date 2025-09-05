#pragma once

#define DCLASS(...)
#define DPROPERTY(...)

#define BODY_MACRO_COMBINE(A, B, C, D) A##B##C##D
//#define GENERATED_BODY(...) BODY_MACRO_COMBINE(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY);
#define GENERATED_BODY(...)