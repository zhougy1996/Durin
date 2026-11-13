#pragma once

#include "CoreMinimal.h"
#include "DObject/WeakObjectPtr.h"
#include "Misc/Name.h"

namespace Durin::AssetPrivate
{
	// Dispatch the existing event after local result application.
	auto PublishCompiledAssets(FName CompilerName, std::vector<FWeakObjectPtr> Assets) -> void;
}
