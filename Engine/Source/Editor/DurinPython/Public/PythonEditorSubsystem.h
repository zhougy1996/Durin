#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "DurinPythonAPI.h"
#include "PythonEditorSubsystem.gen.h"

namespace Durin
{
	class FDurinPythonModule;
	// Attaches the module's optional services to one editor session.
	DCLASS()
	class DPythonEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINPYTHON_API explicit DPythonEditorSubsystem(const FObjectInitializer& Initializer);
		DURINPYTHON_API auto Initialize(FSubsystemCollection& Collection) -> void override;
		DURINPYTHON_API auto Deinitialize() noexcept -> void override;
	private:
		FDurinPythonModule* Module = nullptr;
	};
}
