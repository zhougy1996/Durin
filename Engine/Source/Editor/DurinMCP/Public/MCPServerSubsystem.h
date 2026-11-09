#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "DurinMCPAPI.h"
#include "MCPServerSubsystem.gen.h"

namespace Durin
{
	class FDurinMCPModule;
	// Attaches the module's optional services to one editor session.
	DCLASS(NoClassDefaultObject)
	class DMCPServerSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINMCP_API explicit DMCPServerSubsystem(const FObjectInitializer& Initializer);
		DURINMCP_API auto Initialize() -> FSubsystemResult override;
		DURINMCP_API auto Deinitialize() noexcept -> void override;
	private:
		FDurinMCPModule* Module = nullptr;
	};
}
