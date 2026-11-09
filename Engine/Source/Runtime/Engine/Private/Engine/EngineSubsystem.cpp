#include "Engine/EngineSubsystem.h"
#include "Engine/Engine.h"
#include "DObject/Class.h"

namespace Durin
{
	DEngineSubsystem::DEngineSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEngineSubsystem::BeginDestroy() -> void
	{
		if (!HasCompletedDeinitialization())
			if (auto* Host = GetEngine()) Host->PrepareForShutdown();
		Super::BeginDestroy();
	}
	auto DEngineSubsystem::IsReadyForFinishDestroy() -> bool
	{
		if (HasCompletedDeinitialization()) return true;
		auto* Host = GetEngine();
		return !Host || Host->IsReadyForFinishDestroy();
	}
	auto DEngineSubsystem::GetEngine() const -> DEngine* { return Cast<DEngine>(GetOuter()); }
	FEngineSubsystemCollection::FEngineSubsystemCollection(DEngine& Host) : FSubsystemCollection(Host, DEngineSubsystem::StaticClass()) {}
	auto FEngineSubsystemCollection::Initialize() -> FSubsystemResult
	{
		return FSubsystemCollection::Initialize(FSubsystemRegistration::Snapshot(DEngineSubsystem::StaticClass()));
	}
}
