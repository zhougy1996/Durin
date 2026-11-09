#pragma once

#include "CoreMinimal.h"

#include "Engine/DynamicSubsystem.h"
#include "DurinEdAPI.h"
#include "EditorSubsystem.gen.h"

namespace Durin
{
	class DEditorEngine;

	// One transient service on the active editor; editor shutdown owns retirement.
	DCLASS(Abstract)
	class DEditorSubsystem : public DDynamicSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto GetEditor() const -> DEditorEngine*;
	};

	// Explicit provider token; the active editor reconciles publication changes at safe points.
	class FEditorSubsystemRegistration : public FSubsystemRegistration
	{
	public:
		explicit FEditorSubsystemRegistration(FSubsystemDescriptor Descriptor)
			: FSubsystemRegistration(DEditorSubsystem::StaticClass(), std::move(Descriptor)) {}
	};

	// Dynamic scope adapter selects registrations and reconciles them at editor safe points.
	class FEditorSubsystemCollection : public FDynamicSubsystemCollection
	{
	public:
		DURINED_API explicit FEditorSubsystemCollection(DEditorEngine& Host);
		DURINED_API ~FEditorSubsystemCollection() override;
		DURINED_API auto Initialize() -> FSubsystemResult;
		DURINED_API auto Synchronize() -> FSubsystemResult;
		// Provider removal preflights the remaining descriptor graph without user callbacks.
		DURINED_API static auto CanSynchronizeActiveEditor(FName RemovingProvider = {}) -> bool;
		DURINED_API static auto SynchronizeActiveEditor() -> void;
	};
}
