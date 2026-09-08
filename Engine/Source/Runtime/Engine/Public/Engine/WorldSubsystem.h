#pragma once

#include "EngineAPI.h"
#include "Engine/Subsystem.h"
#include "DObject/ObjectPtr.h"
#include "Engine/TickFunction.h"
#include "Threading/Task.h"
#include "WorldSubsystem.gen.h"

namespace Durin
{
	class DWorld;
	class DLevel;
	enum class EWorldType : uint8;

	using EWorldSubsystemState = ESubsystemState;
	using EWorldSubsystemError = ESubsystemError;
	using FWorldSubsystemResult = FSubsystemResult;
	using FWorldSubsystemWorkGate = FSubsystemWorkGate;

	// Native per-World service. Workers may capture detached data and a gate, never this object.
	DCLASS(Abstract, NoClassDefaultObject)
	class DWorldSubsystem : public DSubsystem
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DWorldSubsystem(const FObjectInitializer& Initializer);
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto GetWorld() const -> DWorld*;
		virtual auto OnWorldBeginPlay() noexcept -> void {}
		virtual auto OnWorldEndPlay() noexcept -> void {}
		virtual auto OnLevelAttached(DLevel&) noexcept -> void {}
		virtual auto OnLevelDetached(DLevel&) noexcept -> void {}
		virtual auto Tick(float DeltaSeconds) noexcept -> void {}
		// Changes are observed at the next World Tick entry.
		ENGINE_API auto SetTickEnabled(bool bEnabled) -> void;
		auto IsTickEnabled() const -> bool { return bTickEnabled; }
	private:
		bool bTickEnabled = true;
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
	class FWorldSubsystemRegistration : public FSubsystemRegistration
	{
	public:
		ENGINE_API explicit FWorldSubsystemRegistration(FWorldSubsystemDescriptor Descriptor);
		ENGINE_API ~FWorldSubsystemRegistration();
		FWorldSubsystemRegistration(const FWorldSubsystemRegistration&) = delete;
		auto operator=(const FWorldSubsystemRegistration&) -> FWorldSubsystemRegistration& = delete;
	};

	// Owns fixed service membership and deterministic dispatch; only DWorld drives callbacks.
	class FWorldSubsystemCollection : public FSubsystemCollection
	{
	public:
		ENGINE_API explicit FWorldSubsystemCollection(DWorld& InWorld);
		ENGINE_API ~FWorldSubsystemCollection();
		FWorldSubsystemCollection(const FWorldSubsystemCollection&) = delete;
		auto operator=(const FWorldSubsystemCollection&) -> FWorldSubsystemCollection& = delete;
		ENGINE_API auto Find(DClass* Type) const -> DWorldSubsystem*;
	private:
		struct FWorldEntry
		{
			FWorldSubsystemDescriptor Descriptor;
			bool bPlaying = false;
			bool bAttached = false;
			bool bFrameTickEnabled = false;
		};
		auto Initialize() -> FWorldSubsystemResult;
		auto BeginPlay() -> void;
		auto EndPlay() -> void;
		auto LevelChanged(DLevel& Level, bool bAttached) -> void;
		auto StartTick() -> void;
		auto Tick(ETickingGroup Group, float DeltaSeconds, bool bGameplay) -> void;
		DWorld& World;
		std::unordered_map<DClass*, FWorldEntry> WorldEntries;
		friend class DWorld;
	};
}
