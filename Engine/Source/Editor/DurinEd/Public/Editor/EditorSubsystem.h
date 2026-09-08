#pragma once

#include "Engine/Subsystem.h"
#include "DurinEdAPI.h"
#include "EditorSubsystem.gen.h"

namespace Durin
{
	class DEditorEngine;

	// One transient service per Editor host; host shutdown owns retirement.
	DCLASS(Abstract, NoClassDefaultObject)
	class DEditorSubsystem : public DSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto BeginDestroy() -> void override;
		DURINED_API auto IsReadyForFinishDestroy() -> bool override;
		DURINED_API auto GetEditor() const -> DEditorEngine*;
	};

	// Explicit provider token for future Editor hosts.
	class FEditorSubsystemRegistration : public FSubsystemRegistration
	{
	public:
		explicit FEditorSubsystemRegistration(FSubsystemDescriptor Descriptor)
			: FSubsystemRegistration(DEditorSubsystem::StaticClass(), std::move(Descriptor)) {}
	};

	// Scope adapter selects native registrations; common code validates and constructs them.
	class FEditorSubsystemCollection : public FSubsystemCollection
	{
	public:
		DURINED_API explicit FEditorSubsystemCollection(DEditorEngine& Host);
		DURINED_API auto Initialize() -> FSubsystemResult;
	};
}
