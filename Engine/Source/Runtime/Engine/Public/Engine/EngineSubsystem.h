#pragma once

#include "Engine/Subsystem.h"
#include "EngineAPI.h"
#include "EngineSubsystem.gen.h"

namespace Durin
{
	class DEngine;

	// One transient service per Engine host; host shutdown owns retirement.
	DCLASS(Abstract, NoClassDefaultObject)
	class DEngineSubsystem : public DSubsystem
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DEngineSubsystem(const FObjectInitializer& Initializer);
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto GetEngine() const -> DEngine*;
	};

	// Explicit provider token for future Engine hosts.
	class FEngineSubsystemRegistration : public FSubsystemRegistration
	{
	public:
		explicit FEngineSubsystemRegistration(FSubsystemDescriptor Descriptor)
			: FSubsystemRegistration(DEngineSubsystem::StaticClass(), std::move(Descriptor)) {}
	};

	// Scope adapter selects native registrations; common code validates and constructs them.
	class FEngineSubsystemCollection : public FSubsystemCollection
	{
	public:
		ENGINE_API explicit FEngineSubsystemCollection(DEngine& Host);
		ENGINE_API auto Initialize() -> FSubsystemResult;
	};
}
