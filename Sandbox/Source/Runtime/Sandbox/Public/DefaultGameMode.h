#pragma once

#include "Actors/GameMode.h"
#include "SandboxAPI.h"

#include "DefaultGameMode.gen.h"

namespace Durin::Sandbox
{
	// Selects the concrete controller and graybox pawn for the Sandbox local-player session.
	DCLASS()
	class ADefaultGameMode final : public AGameMode
	{
		GENERATED_BODY()
	public:
		SANDBOX_API explicit ADefaultGameMode(const FObjectInitializer& ObjectInitializer);
		SANDBOX_API auto GetPlayerControllerClass() const -> DClass* override;
		SANDBOX_API auto GetDefaultPawnClass() const -> DClass* override;
	};
}
