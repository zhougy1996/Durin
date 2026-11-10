#include "Engine/EngineSubsystem.h"
#include "Engine/Engine.h"
#include "DObject/Class.h"

namespace Durin
{
	DEngineSubsystem::DEngineSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEngineSubsystem::GetEngine() const -> DEngine* { return Cast<DEngine>(GetOuter()); }
	FEngineSubsystemCollection::FEngineSubsystemCollection(DEngine& Host) : FSubsystemCollection(Host, DEngineSubsystem::StaticClass()) {}
	auto FEngineSubsystemCollection::Initialize() -> void
	{
		FSubsystemCollection::Initialize(FSubsystemRegistration::GetRegistrations(DEngineSubsystem::StaticClass()));
	}
}
