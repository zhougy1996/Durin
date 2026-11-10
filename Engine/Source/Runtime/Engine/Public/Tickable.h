#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin
{
	class DWorld;

	enum class ETickableTickType : uint8 { Conditional, Always, Never, NewObject };
	enum ELevelTick : uint8 { LEVELTICK_TimeOnly, LEVELTICK_ViewportsOnly, LEVELTICK_All, LEVELTICK_PauseTick };

	// Non-owning game-thread callback contract. Queries must not mutate registration.
	class FTickableObjectBase
	{
	public:
		virtual ~FTickableObjectBase() = default;
		virtual auto GetTickableTickType() const -> ETickableTickType { return ETickableTickType::Conditional; }
		virtual auto IsAllowedToTick() const -> bool { return true; }
		virtual auto IsTickable() const -> bool { return true; }
		virtual auto Tick(float DeltaSeconds) noexcept -> void = 0;
	};

	// Independent of UObject ownership; Never permits construction before game-thread activation.
	class FTickableGameObject : public FTickableObjectBase
	{
	public:
		ENGINE_API explicit FTickableGameObject(ETickableTickType StartingTickType = ETickableTickType::NewObject);
		ENGINE_API ~FTickableGameObject() override;
		FTickableGameObject(const FTickableGameObject&) = delete;
		auto operator=(const FTickableGameObject&) -> FTickableGameObject& = delete;
		FTickableGameObject(FTickableGameObject&&) = delete;
		auto operator=(FTickableGameObject&&) -> FTickableGameObject& = delete;
		virtual auto GetTickableGameObjectWorld() const -> DWorld* { return nullptr; }
		virtual auto IsTickableInEditor() const -> bool { return false; }
		virtual auto IsTickableWhenPaused() const -> bool { return false; }
		ENGINE_API auto SetTickableTickType(ETickableTickType NewTickType) -> void;
		ENGINE_API static auto TickObjects(DWorld* World, ELevelTick TickType, bool bIsPaused, float DeltaSeconds) -> void;
	private:
		bool bRegistered = false;
	};
}
