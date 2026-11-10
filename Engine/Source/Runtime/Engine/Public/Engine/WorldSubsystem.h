#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Engine/Subsystem.h"
#include "Tickable.h"
#include "WorldSubsystem.gen.h"

namespace Durin
{
	class DWorld;
	class DLevel;
	class FSceneInterface;
	enum class EWorldType : uint8;

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
	};

	// Tick lifetime follows initialization, independently of the World's play lifetime.
	DCLASS(Abstract)
	class DTickableWorldSubsystem : public DWorldSubsystem, public FTickableGameObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTickableWorldSubsystem(const FObjectInitializer& Initializer);
		// Overrides must forward to Super to start/stop ticking.
		ENGINE_API auto Initialize(FSubsystemCollection& Collection) -> void override;
		ENGINE_API auto Deinitialize() noexcept -> void override;
		ENGINE_API auto BeginDestroy() -> void override;
		auto IsInitialized() const -> bool { return bInitialized; }
		ENGINE_API auto IsAllowedToTick() const -> bool override final;
		ENGINE_API auto GetTickableTickType() const -> ETickableTickType override;
		auto GetTickableGameObjectWorld() const -> DWorld* override { return GetWorld(); }
		ENGINE_API auto Tick(float DeltaSeconds) noexcept -> void override;
	private:
		bool bInitialized = false;
	};

	// Provider-owned publication token; removal affects only future World snapshots.
	class FWorldSubsystemRegistration : public FSubsystemRegistration
	{
	public:
		explicit FWorldSubsystemRegistration(FSubsystemDescriptor Descriptor)
			: FSubsystemRegistration(DWorldSubsystem::StaticClass(), std::move(Descriptor)) {}
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
		auto Initialize() -> void;
		auto BeginPlay() -> void;
		auto EndPlay() -> void;
		auto LevelChanged(DLevel& Level, bool bAttached) -> void;
		auto RenderSceneChanged(FSceneInterface* Previous, FSceneInterface* Current) -> void;
		friend class DWorld;
	};
}
