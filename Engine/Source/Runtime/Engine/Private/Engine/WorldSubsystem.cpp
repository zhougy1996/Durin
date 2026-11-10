#include "Engine/WorldSubsystem.h"

#include "Engine/World.h"

namespace Durin
{
	DWorldSubsystem::DWorldSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DWorldSubsystem::GetWorld() const -> DWorld* { return Cast<DWorld>(GetOuter()); }

	DTickableWorldSubsystem::DTickableWorldSubsystem(const FObjectInitializer& Initializer)
		: Super(Initializer), FTickableGameObject(ETickableTickType::Never) {}
	auto DTickableWorldSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{
		Super::Initialize(Collection);
		requiref(!bInitialized, "Tickable subsystem initialization must be paired.");
		bInitialized = true;
		SetTickableTickType(GetTickableTickType());
	}
	auto DTickableWorldSubsystem::Deinitialize() noexcept -> void
	{
		Super::Deinitialize();
		requiref(bInitialized, "Tickable subsystem cleanup requires Super::Initialize first.");
		bInitialized = false;
		SetTickableTickType(ETickableTickType::Never);
	}
	auto DTickableWorldSubsystem::BeginDestroy() -> void
	{
		Super::BeginDestroy();
		requiref(!bInitialized, "Tickable subsystem destroyed before Super::Deinitialize.");
	}
	auto DTickableWorldSubsystem::IsAllowedToTick() const -> bool
	{
		requiref(bInitialized, "Tickable subsystem registered before initialization.");
		return bInitialized;
	}

	auto DTickableWorldSubsystem::GetTickableTickType() const -> ETickableTickType
	{
		return IsClassDefaultObject() || !bInitialized ? ETickableTickType::Never : ETickableTickType::Conditional;
	}

	auto DTickableWorldSubsystem::Tick(float) noexcept -> void
	{
		requiref(bInitialized, "Ticking an uninitialized subsystem.");
	}

	auto DWorldSubsystem::ShouldCreateSubsystem(DObject* Outer) const -> bool
	{
		const auto* World = Cast<DWorld>(Outer);
		return Super::ShouldCreateSubsystem(Outer) && World && DoesSupportWorldType(World->GetWorldType());
	}

	auto DWorldSubsystem::DoesSupportWorldType(EWorldType WorldType) const -> bool
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::Editor || WorldType == EWorldType::PIE;
	}

	FWorldSubsystemCollection::FWorldSubsystemCollection(DWorld& InWorld) : FSubsystemCollection(InWorld, DWorldSubsystem::StaticClass()) {}
	FWorldSubsystemCollection::~FWorldSubsystemCollection() = default;

	auto FWorldSubsystemCollection::Find(DClass* Type) const -> DWorldSubsystem*
	{
		return static_cast<DWorldSubsystem*>(FSubsystemCollection::Find(Type));
	}

	auto FWorldSubsystemCollection::Initialize() -> void
	{
		FSubsystemCollection::Initialize(FSubsystemRegistration::GetRegistrations(DWorldSubsystem::StaticClass()));
	}

	auto FWorldSubsystemCollection::BeginPlay() -> void
	{
		// The World owns admission and pairs complete lifecycle notification batches.
		for (auto& Entry : Entries)
			static_cast<DWorldSubsystem*>(Entry.Object.Get())->OnWorldBeginPlay();
	}
	auto FWorldSubsystemCollection::EndPlay() -> void
	{
		for (size_t Index = Entries.size(); Index-- > 0;)
			static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnWorldEndPlay();
	}
	auto FWorldSubsystemCollection::LevelChanged(DLevel& Level, bool bAttached) -> void
	{
		for (size_t Offset = 0; Offset < Entries.size(); ++Offset)
		{
			const size_t Index = bAttached ? Offset : Entries.size() - Offset - 1;
			if (bAttached) static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnLevelAttached(Level);
			else static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnLevelDetached(Level);
		}
	}
	auto FWorldSubsystemCollection::RenderSceneChanged(
		FSceneInterface* Previous, FSceneInterface* Current) -> void
	{
		for (auto& Entry : Entries)
			static_cast<DWorldSubsystem*>(Entry.Object.Get())->OnRenderSceneChanged(Previous, Current);
	}
}
