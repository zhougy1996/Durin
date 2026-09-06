#pragma once
#include "Engine/WorldSubsystem.h"
#include "Collision/CollisionTypes.h"
#include "CollisionDebugSubsystem.gen.h"

namespace Durin
{
	// Keeps diagnostic state local to a World and releases Level-derived hits on detachment.
	DCLASS(NoClassDefaultObject)
	class DCollisionDebugSubsystem : public DWorldSubsystem
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DCollisionDebugSubsystem(const FObjectInitializer& Initializer);
		auto IsEnabled() const -> bool { return bEnabled; }
		auto SetEnabled(bool bValue) -> void { bEnabled = bValue; if (!bEnabled) LastHit.reset(); }
		auto RecordHit(const FHitResult& Hit) -> void { if (bEnabled) LastHit = Hit; }
		auto GetLastHit() const -> const std::optional<FHitResult>& { return LastHit; }
		auto OnLevelDetached(DLevel&) noexcept -> void override { LastHit.reset(); }
		auto Deinitialize() noexcept -> void override { SetEnabled(false); }
	private:
		bool bEnabled = false;
		std::optional<FHitResult> LastHit;
	};
}
