#include "NativeGameplayLifecycleSmoke.h"

#include "Actors/GameMode.h"
#include "Actors/PlayerStart.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineGlobals.h"

namespace Durin
{
	auto RunNativeGameplayLifecycleSmoke() -> void
	{
		DWorld* OriginalWorld = GEngine ? GEngine->GetWorld() : nullptr;
		checkf(GEngine && OriginalWorld,
			"Native gameplay lifecycle smoke requires an initialized Engine World.");
		if (!GEngine || !OriginalWorld) return;

		auto* SmokeWorld = NewObject<DWorld>(GEngine, "NativeGameplayLifecycleSmokeWorld");
		auto* SmokeLevel = NewObject<DLevel>(SmokeWorld, "NativeGameplayLifecycleSmokeLevel");
		GEngine->SetWorld(SmokeWorld);
		const auto Restore = [&] {
			SmokeWorld->EndPlay();
			GEngine->SetWorld(OriginalWorld);
			MarkObjectHierarchyAsGarbage(SmokeWorld);
		};
		if (!SmokeWorld->SetCurrentLevel(SmokeLevel)
			|| !SmokeLevel->SpawnActor<APlayerStart>("Start"))
		{
			Restore();
			checkf(false, "Native gameplay lifecycle smoke could not prepare its World.");
			return;
		}

		const FWorldPlayResult PlayResult = SmokeWorld->BeginPlay({
			.GameModeClass = AGameMode::StaticClass()});
		if (!PlayResult || !SmokeWorld->GetGameMode()
			|| !SmokeWorld->GetLocalPlayerController() || !SmokeWorld->GetDefaultPawn())
		{
			const std::string Error = PlayResult.Message;
			Restore();
			checkf(false, "Native gameplay lifecycle smoke could not start: {}", Error);
			return;
		}
		SmokeWorld->Tick({.DeltaSeconds = 1.0f / 60.0f,
			.GameInput = &GEngine->GetGameInputState()});
		SmokeWorld->SetPaused(true);
		SmokeWorld->Tick({.DeltaSeconds = 1.0f / 60.0f,
			.GameInput = &GEngine->GetGameInputState()});
		SmokeWorld->RequestSingleStep();
		SmokeWorld->Tick({.DeltaSeconds = 1.0f / 60.0f,
			.GameInput = &GEngine->GetGameInputState()});
		SmokeWorld->SetPaused(false);
		const FPlayerRestartResult Restart = SmokeWorld->RestartPlayer();
		if (!Restart)
		{
			const std::string Error = Restart.Message;
			Restore();
			checkf(false, "Native gameplay lifecycle smoke could not restart: {}", Error);
			return;
		}
		Restore();
		DURIN_INFO("Native gameplay lifecycle smoke completed start, tick, step, restart, and stop.");
	}
}
