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

	// Explicit provider token; the active editor applies explicit additions and removals at safe points.
	class FEditorSubsystemRegistration : public FSubsystemRegistration
	{
	public:
		explicit FEditorSubsystemRegistration(FSubsystemDescriptor Descriptor)
			: FSubsystemRegistration(DEditorSubsystem::StaticClass(), std::move(Descriptor)) {}
	};

	// Dynamic scope adapter selects registrations and adds them at editor safe points.
	class FEditorSubsystemCollection : public FDynamicSubsystemCollection
	{
	public:
		DURINED_API explicit FEditorSubsystemCollection(DEditorEngine& Host);
		DURINED_API ~FEditorSubsystemCollection() override;
		DURINED_API auto Initialize() -> void;
		DURINED_API auto AddRegisteredSubsystems() -> void;
		DURINED_API auto AddSubsystem(DClass* Type) -> DEditorSubsystem*;
		DURINED_API auto RemoveSubsystem(DClass* Type) -> void;
		DURINED_API auto RemoveSubsystems(FName Provider) -> void;
	private:
		auto HasProvider(FName Provider) const -> bool
		{ return std::ranges::any_of(Entries, [Provider](const auto& Entry) { return Entry.Descriptor.Provider == Provider; }); }
		friend class DEditorEngine;
	};
}
