#pragma once

#include "CoreMinimal.h"
#include <gtest/gtest.h>
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/Task.h"

namespace Durin
{
	class FTaskCloseTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
			ShutdownTaskScheduler(false);
		}
		auto TearDown() -> void override { ShutdownTaskScheduler(false); }
	};
}
