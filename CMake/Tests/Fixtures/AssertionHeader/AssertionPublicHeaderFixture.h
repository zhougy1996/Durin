#pragma once

#include "Misc/AssertionMacros.h"

inline auto ExerciseAssertionPublicHeader(bool bCondition) -> void
{
	checkf(bCondition, "Public assertion header fixture condition was false.");
	requiref(bCondition, "Public required-contract header fixture condition was false.");
}
