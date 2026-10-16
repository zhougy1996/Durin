#pragma once

#include "CoreMinimal.h"
#include "DurinMCPAPI.h"

namespace Durin
{
	class DEditorEngine;
	class FPythonRuntime;
	struct FMCPServerOptions
	{
		uint16 Port = 0;
		std::string BearerToken;
		std::chrono::milliseconds CallTimeout{30000};
		// Optional, already-started owner runtime; server owns only per-client sessions/jobs.
		std::shared_ptr<FPythonRuntime> PythonRuntime;
	};
	// One explicitly selected editor; control-thread start/stop, worker-owned transport.
	class FMCPServer final
	{
	public:
		DURINMCP_API FMCPServer();
		DURINMCP_API ~FMCPServer();
		FMCPServer(const FMCPServer&) = delete;
		auto operator=(const FMCPServer&) -> FMCPServer& = delete;
		DURINMCP_API auto Start(DEditorEngine& Editor, const FMCPServerOptions& Options) -> std::expected<uint16, std::string>;
		DURINMCP_API auto Stop() noexcept -> void;
	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
