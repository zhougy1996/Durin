#pragma once
#include "CoreMinimal.h"
namespace Durin::MaterialBuildPrivate
{
	// Invoked after detaching a completion, outside the compilation-state lock.
	ENGINE_API auto SetResultFanoutHookForTests(std::function<void()> Hook) -> void;
}
