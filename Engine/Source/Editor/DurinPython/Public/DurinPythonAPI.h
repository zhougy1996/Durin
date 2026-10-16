#pragma once
#include "CoreMinimal.h"
#if defined(DURINPYTHON_EXPORTS)
#define DURINPYTHON_API DLLEXPORT
#else
#define DURINPYTHON_API DLLIMPORT
#endif
