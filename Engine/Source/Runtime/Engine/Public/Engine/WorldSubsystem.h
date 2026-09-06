#pragma once

#include "EngineAPI.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "Engine/TickFunction.h"
#include "Threading/Task.h"
#include "WorldSubsystem.gen.h"

namespace Durin
{
	class DWorld;
	class DLevel;
	enum class EWorldType : uint8;

	// Separates one-shot World service lifetime from repeatable play lifetimes.
	enum class EWorldSubsystemState : uint8 { Uninitialized, Initializing, Ready, ShuttingDown, Shutdown, Failed };
	enum class EWorldSubsystemError : uint8 { None, InvalidState, InvalidDescriptor, DuplicateType, MissingDependency, DependencyCycle, ProviderUnavailable, InitializationFailed, Aborted };
	struct FWorldSubsystemResult
	{
		EWorldSubsystemError Error = EWorldSubsystemError::None;
		std::string Message;
		explicit operator bool() const { return Error == EWorldSubsystemError::None; }
	};

	// A closed gate survives the service; detached completions must test it on the game thread.
	class FWorldSubsystemWorkGate
	{
	public:
		auto IsOpen() const -> bool { return !Cancellation.IsCancellationRequested(); }
		auto GetCancellationToken() const -> FTaskCancellationToken { return Cancellation.GetToken(); }
	private:
		FTaskCancellationSource Cancellation;
		std::shared_ptr<void> ProviderLease;
		std::shared_ptr<void> RuntimeLease;
		friend class FWorldSubsystemCollection;
	};

	// Native per-World service. Workers may capture detached data and a gate, never this object.
	DCLASS(Abstract, NoClassDefaultObject)
	class DWorldSubsystem : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DWorldSubsystem(const FObjectInitializer& Initializer);
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto GetWorld() const -> DWorld*;
		// Deinitialize is paired even with a failed Initialize; cleanup must be idempotent.
		virtual auto Initialize() -> FWorldSubsystemResult { return {}; }
		virtual auto Deinitialize() noexcept -> void {}
		virtual auto OnWorldBeginPlay() noexcept -> void {}
		virtual auto OnWorldEndPlay() noexcept -> void {}
		virtual auto OnLevelAttached(DLevel&) noexcept -> void {}
		virtual auto OnLevelDetached(DLevel&) noexcept -> void {}
		virtual auto Tick(float DeltaSeconds) noexcept -> void {}
		// Changes are observed at the next World Tick entry.
		ENGINE_API auto SetTickEnabled(bool bEnabled) -> void;
		auto IsTickEnabled() const -> bool { return bTickEnabled; }
		auto GetWorkGate() const -> std::shared_ptr<const FWorldSubsystemWorkGate> { return WorkGate; }
	private:
		bool bTickEnabled = true;
		std::shared_ptr<FWorldSubsystemWorkGate> WorkGate;
		// Survives deinitialization until the provider's virtual destructor has retired.
		std::shared_ptr<void> ProviderLease;
		friend class FWorldSubsystemCollection;
	};

	// Describes a concrete native factory and eligibility without process-wide service instances.
	struct FWorldSubsystemDescriptor
	{
		DClass* Type = nullptr;
		FName Provider;
		// Empty selects every World type.
		std::vector<EWorldType> WorldTypes;
		std::vector<DClass*> Dependencies;
		bool bTick = false;
		bool bTickInEditorAndPreview = false;
		ETickingGroup TickGroup = ETickingGroup::PrePhysics;
	};

	// Provider-owned publication token; removal affects only future World snapshots.
	class FWorldSubsystemRegistration
	{
	public:
		ENGINE_API explicit FWorldSubsystemRegistration(FWorldSubsystemDescriptor Descriptor);
		ENGINE_API ~FWorldSubsystemRegistration();
		FWorldSubsystemRegistration(const FWorldSubsystemRegistration&) = delete;
		auto operator=(const FWorldSubsystemRegistration&) -> FWorldSubsystemRegistration& = delete;
	private:
		uint64 Identity;
	};

	// Owns fixed service membership and deterministic dispatch; only DWorld drives callbacks.
	class FWorldSubsystemCollection
	{
	public:
		ENGINE_API explicit FWorldSubsystemCollection(DWorld& InWorld);
		ENGINE_API ~FWorldSubsystemCollection();
		FWorldSubsystemCollection(const FWorldSubsystemCollection&) = delete;
		auto operator=(const FWorldSubsystemCollection&) -> FWorldSubsystemCollection& = delete;
		ENGINE_API auto Find(DClass* Type) const -> DWorldSubsystem*;
		auto GetState() const -> EWorldSubsystemState { return State; }
	private:
		struct FEntry
		{
			FWorldSubsystemDescriptor Descriptor;
			std::shared_ptr<void> Lease;
			TObjectPtr<DWorldSubsystem> Object;
			bool bInitialized = false;
			bool bPlaying = false;
			bool bAttached = false;
			bool bFrameTickEnabled = false;
		};
		auto Initialize() -> FWorldSubsystemResult;
		auto CloseWork() -> void;
		auto Shutdown() -> void;
		auto AddReferencedObjects(FReferenceCollector& Collector) -> void;
		auto BeginPlay() -> void;
		auto EndPlay() -> void;
		auto LevelChanged(DLevel& Level, bool bAttached) -> void;
		auto StartTick() -> void;
		auto Tick(ETickingGroup Group, float DeltaSeconds, bool bGameplay) -> void;
		DWorld& World;
		EWorldSubsystemState State = EWorldSubsystemState::Uninitialized;
		std::vector<FEntry> Entries;
		friend class DWorld;
	};
}
