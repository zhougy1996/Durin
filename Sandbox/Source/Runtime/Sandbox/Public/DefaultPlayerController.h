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
		// Persist first, then publish the new mapping. Failure leaves live controls intact.
		SANDBOX_API auto RebindControl(std::string_view Slot, FInputSource Source, std::string& Error) -> bool;
		SANDBOX_API auto ResetControlBindings(std::string& Error) -> bool;
		SANDBOX_API static auto GetControlBindingsPath() -> std::filesystem::path;

	protected:
		SANDBOX_API auto BeginPlay() -> void override;
		SANDBOX_API auto BuildControlIntent(const FInputActionSnapshot& Input) const -> FPawnControlIntent override;

	private:
		friend struct FDefaultPlayerControllerTestAccess;
	};
}
