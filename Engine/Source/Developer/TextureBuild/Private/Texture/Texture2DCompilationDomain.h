#pragma once

#include "Modules/ModularFeature.h"
#include "Texture/Texture2DCompilationScheduler.h"

namespace Durin::Asset::Private
{
	TEXTUREBUILD_API auto InitializeTexture2DCompilationDomain(
		FModuleOwnedCallbackGate OwnerGate,
		const FTexture2DCompilationSchedulerConfig& Config = {}) -> bool;
	TEXTUREBUILD_API auto ShutdownTexture2DCompilationDomain() -> void;
	TEXTUREBUILD_API auto SetTexture2DCompilationPhaseHookForTests(
		std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void;
}
