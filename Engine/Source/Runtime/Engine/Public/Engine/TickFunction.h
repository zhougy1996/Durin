#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DLevel;

	// Defines the serial phase in which one registered Tick function executes.
	enum class ETickingGroup : uint8
	{
		PrePhysics,
		Physics,
		PostPhysics,
		Count
	};

	class FTickRegistry;

	// Provides stable scheduling identity for one game-thread Tick callback.
	class FTickFunction
	{
	public:
		ENGINE_API FTickFunction();
		ENGINE_API virtual ~FTickFunction();

		FTickFunction(const FTickFunction&) = delete;
		auto operator=(const FTickFunction&) -> FTickFunction& = delete;
		FTickFunction(FTickFunction&&) = delete;
		auto operator=(FTickFunction&&) -> FTickFunction& = delete;

		// Registers this stable node with one Level. Calls must remain on the game thread.
		ENGINE_API auto RegisterTickFunction(DLevel* Level) -> void;
		// Cancels pending execution and removes future scheduling without destroying the target.
		ENGINE_API auto UnregisterTickFunction() -> void;
		ENGINE_API auto SetTickFunctionEnable(bool bEnabled) -> void;
		// Changes the serial phase only while no frame is active in the owning registry.
		ENGINE_API auto SetTickGroup(ETickingGroup Group) -> bool;
		ENGINE_API auto CancelPendingTick() -> void;
		ENGINE_API auto NotifyEligibilityChanged() -> void;

		auto IsTickFunctionRegistered() const -> bool { return Registry != nullptr; }
		auto IsTickFunctionEnabled() const -> bool { return bEnabled; }
		auto GetTickGroup() const -> ETickingGroup { return TickGroup; }

	protected:
		virtual auto CanExecute(const DLevel& Level) const -> bool = 0;
		virtual auto ExecuteTick(float DeltaSeconds) -> void = 0;
		virtual auto GetPrerequisite() const -> FTickFunction* { return nullptr; }

	private:
		FTickRegistry* Registry = nullptr;
		ETickingGroup TickGroup = ETickingGroup::PrePhysics;
		uint64 RegistrationOrder = 0;
		uint64 QueuedFrame = 0;
		uint64 ExecutedFrame = 0;
		bool bEnabled = false;
		bool bCancelled = false;
		bool bExecuting = false;
		bool bVisiting = false;

		friend class FTickRegistry;
	};

	// Routes a registered primary Tick function to one Actor.
	class FActorTickFunction final : public FTickFunction
	{
	public:
		auto SetTarget(AActor* InTarget) -> void { Target = InTarget; }
		auto GetTarget() const -> AActor* { return Target; }

	protected:
		auto CanExecute(const DLevel& Level) const -> bool override;
		auto ExecuteTick(float DeltaSeconds) -> void override;

	private:
		AActor* Target = nullptr;
	};

	// Routes a registered primary Tick function to one Actor Component.
	class FActorComponentTickFunction final : public FTickFunction
	{
	public:
		auto SetTarget(DActorComponent* InTarget) -> void { Target = InTarget; }
		auto GetTarget() const -> DActorComponent* { return Target; }

	protected:
		auto CanExecute(const DLevel& Level) const -> bool override;
		auto ExecuteTick(float DeltaSeconds) -> void override;
		auto GetPrerequisite() const -> FTickFunction* override;

	private:
		DActorComponent* Target = nullptr;
	};

	// Owns one Level's non-owning Tick registrations and serial frame queues.
	class FTickRegistry
	{
	public:
		ENGINE_API explicit FTickRegistry(DLevel* InLevel = nullptr);
		ENGINE_API ~FTickRegistry();

		FTickRegistry(const FTickRegistry&) = delete;
		auto operator=(const FTickRegistry&) -> FTickRegistry& = delete;

		// Seals the frame baseline and admits currently eligible registered nodes.
		ENGINE_API auto StartFrame(float InDeltaSeconds) -> void;
		// Executes one later serial group and returns false when World admission is lost.
		ENGINE_API auto RunTickGroup(ETickingGroup Group) -> bool;
		ENGINE_API auto EndFrame() -> void;
		// Detaches every non-owning node before the Level leaves its World.
		ENGINE_API auto Reset() -> void;

		auto IsTicking() const -> bool { return bTicking; }
		auto GetRegisteredTickCount() const -> size_t { return RegisteredTicks.size(); }

	private:
		auto Register(FTickFunction& TickFunction) -> void;
		auto Unregister(FTickFunction& TickFunction) -> void;
		auto OnEnabledChanged(FTickFunction& TickFunction) -> void;
		auto Cancel(FTickFunction& TickFunction) -> void;
		auto TryQueue(FTickFunction& TickFunction) -> void;
		auto ExecuteQueued(FTickFunction& TickFunction) -> bool;
		auto CheckGameThreadContract() const -> void;

		DLevel* Level = nullptr;
		std::vector<FTickFunction*> RegisteredTicks;
		std::array<std::vector<FTickFunction*>, static_cast<size_t>(ETickingGroup::Count)> GroupQueues;
		uint64 NextRegistrationOrder = 1;
		uint64 Frame = 0;
		float DeltaSeconds = 0.0f;
		int32 CurrentGroup = -1;
		bool bTicking = false;

		friend class FTickFunction;
	};
} // namespace Durin
