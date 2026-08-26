#pragma once

#include "Modules/ModularFeature.h"
#include "Texture/Texture2DBuildScheduler.h"

namespace Durin::Asset::Private
{
	TEXTUREBUILD_API auto InitializeTexture2DCompilingDomain(
		FModuleOwnedCallbackGate OwnerGate,
		const FTexture2DBuildSchedulerConfig& Config = {}) -> bool;
	TEXTUREBUILD_API auto ShutdownTexture2DCompilingDomain() -> void;
	TEXTUREBUILD_API auto SetTexture2DBuildPhaseHookForTests(
		std::function<void(uint64, ETexture2DBuildPhase)> Hook) -> void;
}
