#pragma once

#include "CoreMinimal.h"

#include "Engine/Subsystem.h"
#include "EngineAPI.h"
#include "DynamicSubsystem.gen.h"

namespace Durin
{
	// Opts a service scope into membership changes during its host's lifetime.
	DCLASS(Abstract)
	class DDynamicSubsystem : public DSubsystem
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DDynamicSubsystem(const FObjectInitializer& Initializer);
	};

	// Dynamic scopes reconcile registrations only at host-controlled safe points.
	class FDynamicSubsystemCollection : public FSubsystemCollection
	{
	public:
		ENGINE_API FDynamicSubsystemCollection(DObject& InHost, DClass* InScope);
		ENGINE_API ~FDynamicSubsystemCollection() override;
	protected:
		ENGINE_API auto Reconcile(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult;
		// Read-only metadata preflight; never invokes subsystem eligibility or lifecycle callbacks.
		ENGINE_API auto CanRemoveProvider(FName Provider) const -> bool;
	};
}
