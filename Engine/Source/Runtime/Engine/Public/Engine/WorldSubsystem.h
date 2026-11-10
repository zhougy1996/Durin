#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Engine/Subsystem.h"
#include "DObject/ObjectPtr.h"
#include "Engine/TickFunction.h"
#include "WorldSubsystem.gen.h"

namespace Durin
{
	class DWorld;
	class DLevel;
	class FSceneInterface;
	enum class EWorldType : uint8;

	using EWorldSubsystemState = ESubsystemState;

	// Native per-World service. Workers capture detached data and consumer-owned cancellation state, never this object.
	DCLASS(Abstract)
	class DWorldSubsystem : public DSubsystem
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DWorldSubsystem(const FObjectInitializer& Initializer);
		ENGINE_API auto GetWorld() const -> DWorld*;
		ENGINE_API auto ShouldCreateSubsystem(DObject* Outer) const -> bool override;
		ENGINE_API virtual auto DoesSupportWorldType(EWorldType WorldType) const -> bool;
		virtual auto OnWorldBeginPlay() noexcept -> void {}
		virtual auto OnWorldEndPlay() noexcept -> void {}
		virtual auto OnLevelAttached(DLevel&) noexcept -> void {}
		virtual auto OnLevelDetached(DLevel&) noexcept -> void {}
		virtual auto OnRenderSceneChanged(FSceneInterface*, FSceneInterface*) noexcept -> void {}
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
		ENGINE_API static auto GetRegistrations() -> std::vector<FWorldSubsystemDescriptor>;
	private:
		uint64 Identity;
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
			bool bTick = false;
			bool bTickInEditorAndPreview = false;
			ETickingGroup TickGroup = ETickingGroup::PrePhysics;
			bool bPlaying = false;
			bool bAttached = false;
			bool bFrameTickEnabled = false;
		};
		auto Initialize() -> void;
		auto ValidateSelectedDescriptor(const FSubsystemDescriptor& Descriptor) const -> void override;
		auto BeginPlay() -> void;
		auto EndPlay() -> void;
		auto LevelChanged(DLevel& Level, bool bAttached) -> void;
		auto RenderSceneChanged(FSceneInterface* Previous, FSceneInterface* Current) -> void;
		auto StartTick() -> void;
		auto Tick(ETickingGroup Group, float DeltaSeconds, bool bGameplay) -> void;
		DWorld& World;
		std::unordered_map<DClass*, FWorldEntry> WorldEntries;
		friend class DWorld;
	};
}
