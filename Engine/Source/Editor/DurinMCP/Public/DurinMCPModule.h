#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "MCPServer.h"

namespace Durin
{
	class FDurinMCPModule final : public IModuleInterface
	{
	public:
		DURINMCP_API auto StartForEditor(DEditorEngine& Editor) -> void;
		DURINMCP_API auto StopServer() noexcept -> void;
		auto ShutdownModule() -> void override { StopServer(); }
	private:
		FMCPServer Server;
	};
}
