#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/Object.h"
#include "Engine/TickFunction.h"

#include "ActorComponent.gen.h"

namespace Durin
{
	class AActor;
	struct FActorComponentInstanceData;

	// Records whether component state is native-authored, instance-authored, or transient derived output.
	DENUM()
	enum class EComponentCreationMethod : uint8
	{
		Native,
		Instance,
		Generated
	};

	enum class EComponentPlayState : uint8
	{
		NotBegun,
		BeginningPlay,
		Playing,
		EndingPlay
	};

	enum class EComponentDestructionState : uint8
	{
		Alive,
		Requested,
		Destroying
	};

	struct FActorComponentLifecycleState
	{
		FTickFunctionState Tick;
		bool bCreated = false, bRegistered = false, bInitialized = false, bPlaying = false;
		auto operator==(const FActorComponentLifecycleState&) const -> bool = default;
	};

	// Defines actor-owned lifecycle, registration, play, and optional tick behavior.
	DCLASS(ScriptType)
	class DActorComponent : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API DActorComponent(const FObjectInitializer& ObjectInitializer);

		ENGINE_API virtual ~DActorComponent() override = default;

		auto GetOwner() const -> AActor* { return OwnerActorPrivate; }

		template<typename T> auto GetOwner() const -> T*
		{
			return Cast<T>(GetOuter());
		}

		ENGINE_API auto CaptureLifecycleState() const -> FActorComponentLifecycleState;
		ENGINE_API auto ValidateLifecycleState(const FActorComponentLifecycleState& State) const -> std::expected<void, std::string>;
		ENGINE_API auto ApplyLifecycleState(const FActorComponentLifecycleState& State) -> std::expected<void, std::string>;
		ENGINE_API static auto SerializeLifecycleState(FArchive& Archive, FActorComponentLifecycleState& State) -> void;
		ENGINE_API auto ReregisterComponent() -> void;
		auto HasBeenCreated() const -> bool { return bHasBeenCreated; }
		auto HasBeenInitialized() const -> bool { return bHasBeenInitialized; }

		ENGINE_API auto RegisterComponent() -> void;

		ENGINE_API auto UnregisterComponent() -> void;

		ENGINE_API auto DestroyComponent() -> void;

		ENGINE_API auto BeginDestroy() -> void override;
		using DObject::PostEditUndo;
		ENGINE_API virtual auto GetComponentInstanceData() const -> std::shared_ptr<FActorComponentInstanceData>;
		ENGINE_API auto FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation> override;
		ENGINE_API auto PostEditUndo(std::shared_ptr<ITransactionObjectAnnotation> Annotation) -> void override;

		auto IsRegistered() const -> bool { return bRegistered; }
		auto GetCreationMethod() const -> EComponentCreationMethod { return CreationMethod; }
		auto IsOwnedByActor() const -> bool { return bOwnedByActor; }
		// Advances after every successful registration or unregistration transition.
		auto GetRegistrationGeneration() const -> uint64 { return RegistrationGeneration; }
		auto HasBegunPlay() const -> bool { return PlayState != EComponentPlayState::NotBegun; }
		auto IsBeginningPlay() const -> bool { return PlayState == EComponentPlayState::BeginningPlay; }
		auto IsEndingPlay() const -> bool { return PlayState == EComponentPlayState::EndingPlay; }
		auto IsBeingDestroyed() const -> bool { return DestructionState != EComponentDestructionState::Alive; }
		auto IsComponentTickEnabled() const -> bool { return PrimaryComponentTick.IsTickFunctionEnabled(); }
		auto SetComponentTickEnabled(bool bEnabled) -> void { PrimaryComponentTick.SetTickFunctionEnable(bEnabled); }
		auto SetComponentTickGroup(ETickingGroup Group) -> bool { return PrimaryComponentTick.SetTickGroup(Group); }
		auto GetPrimaryComponentTick() -> FActorComponentTickFunction& { return PrimaryComponentTick; }
		auto GetPrimaryComponentTick() const -> const FActorComponentTickFunction& { return PrimaryComponentTick; }

		ENGINE_API auto DispatchBeginPlay() -> void;
		ENGINE_API auto RouteEndPlay() -> void;
		ENGINE_API virtual auto BeginPlay() -> void;
		ENGINE_API virtual auto TickComponent(float DeltaSeconds) -> void;
		ENGINE_API virtual auto EndPlay() -> void;

		ENGINE_API virtual auto InitializeComponent() -> void;

		ENGINE_API virtual auto UninitializeComponent() -> void;

		ENGINE_API virtual auto OnRegister() -> void;

		ENGINE_API virtual auto OnUnregister() -> void;

		ENGINE_API virtual auto OnComponentCreated() -> void;

		ENGINE_API virtual auto OnComponentDestroyed() -> void;

		ENGINE_API virtual auto OnComponentPendingKill() -> void;

		ENGINE_API virtual auto OnOwnerVisibilityChanged() -> void;

	private:
		auto RegisterComponentInternal(bool bInitialize) -> void;

		// Call OnRegister()
		auto ExecuteRegisterEvents() -> void;

		// Call OnUnregister();
		auto ExecuteUnregisterEvents() -> void;
		auto SetOwnedByActor(bool bOwned) -> void { bOwnedByActor = bOwned; }
		auto SetCreationMethod(EComponentCreationMethod Method) -> void { CreationMethod = Method; }

	private:
		AActor* OwnerActorPrivate;

		DPROPERTY()
		EComponentCreationMethod CreationMethod = EComponentCreationMethod::Native;

		uint8 bRegistered : 1 = false;

		uint8 bHasBeenCreated : 1 = false;

		uint8 bHasBeenInitialized : 1 = false;
		uint8 bOwnedByActor : 1 = false;

		EComponentPlayState PlayState = EComponentPlayState::NotBegun;
		EComponentDestructionState DestructionState = EComponentDestructionState::Alive;
		uint64 RegistrationGeneration = 0;
		bool bEndPlayRequested = false;
		FActorComponentTickFunction PrimaryComponentTick;

		friend class AActor;
		friend class FComponentReregisterContext;
		friend class DLevel;
	};
}
