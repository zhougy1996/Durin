#pragma once

#include "Actors/PlayerController.h"
#include "SandboxAPI.h"

#include "DefaultPlayerController.gen.h"

namespace Durin::Sandbox
{
	struct FDefaultPlayerControllerTestAccess;

	// Maps the first Sandbox keyboard and mouse policy into source-neutral pawn intent.
	DCLASS()
	class ADefaultPlayerController final : public APlayerController
	{
		GENERATED_BODY()
	public:
		SANDBOX_API explicit ADefaultPlayerController(const FObjectInitializer& ObjectInitializer);

	protected:
		SANDBOX_API auto BuildControlIntent(const FGameInputState& Input) const -> FPawnControlIntent override;

	private:
		friend struct FDefaultPlayerControllerTestAccess;
	};
}
