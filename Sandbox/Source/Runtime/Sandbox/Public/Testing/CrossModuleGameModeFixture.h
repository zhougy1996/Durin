#pragma once

#include "Actors/GameMode.h"
#include "SandboxAPI.h"

#include "CrossModuleGameModeFixture.gen.h"

namespace Durin::Sandbox::Testing
{
	// Qualification-only fixture; Sandbox gameplay policy remains deferred to G2.
	DCLASS()
	class ACrossModuleGameModeFixture final : public AGameMode
	{
		GENERATED_BODY()
	public:
		SANDBOX_API explicit ACrossModuleGameModeFixture(const FObjectInitializer& ObjectInitializer);
	};
}
