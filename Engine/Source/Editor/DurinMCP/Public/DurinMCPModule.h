#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "MCPServer.h"
#include "Editor/EditorSubsystem.h"

namespace Durin
{
	class FDurinMCPModule final : public IModuleInterface
	{
	public:
		DURINMCP_API auto StartupModule() -> void override;
		DURINMCP_API auto CanShutdownModule() const -> bool override;
		DURINMCP_API auto ShutdownModule() -> void override;
	private:
		friend class DMCPServerSubsystem;
		auto StartForEditor(DEditorEngine& Editor) -> void;
		auto StopServer() noexcept -> void;
		std::unique_ptr<FEditorSubsystemRegistration> EditorRegistration;
		friend struct FMCPModuleTestAccess;
		uint16 BoundPort = 0;
		FMCPServer Server;
	};
}
